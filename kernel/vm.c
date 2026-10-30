#include "param.h"
#include "types.h"
#include "x86.h"
#include "memlayout.h"
#include "elf.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[]; // kernel.ld sets this to end of kernel code.

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t)kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // x86_64: RAM is mapped at KERNBASE + pa instead of at pa
  // (riscv: identity map), so each mapping passes the virtual
  // address and V2P() of it. COM1 and IDE are I/O ports and need
  // no mapping (riscv: UART0, VIRTIO0, PLIC), and there is no
  // trampoline.

  // IOAPIC and LAPIC registers, one page each.
  // x86_64: these replace the PLIC. They are MMIO above 2GB
  // physical, outside the KERNBASE window, so they go in the
  // device window at DEV2V(pa), and pa is already physical (no
  // V2P). Device registers change on their own and writes to
  // them are commands (e.g. LAPIC EOI), so PCD | PWT makes the
  // pages uncacheable, as Linux's ioremap() does; XD because
  // they are never code.
  kvmmap(kpgtbl, DEV2V(IOAPIC_PA), IOAPIC_PA, PGSIZE,
         PTE_RW | PTE_XD | PTE_PCD | PTE_PWT);
  kvmmap(kpgtbl, DEV2V(LAPIC_PA), LAPIC_PA, PGSIZE,
         PTE_RW | PTE_XD | PTE_PCD | PTE_PWT);

  // map kernel text executable and read-only.
  // x86_64: x86 has no R bit (present implies readable) and
  // pages are executable unless XD is set, so perm is 0.
  // read-only is not enforced for the kernel itself while
  // CR0.WP is 0.
  kvmmap(kpgtbl, KERNBASE, V2P(KERNBASE), (uint64)etext - KERNBASE, 0);

  // map kernel data and the physical RAM we'll make use of.
  // x86_64: XD keeps data from being executed.
  kvmmap(kpgtbl, (uint64)etext, V2P(etext),
         (uint64)P2V(PHYSTOP) - (uint64)etext, PTE_RW | PTE_XD);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);

  return kpgtbl;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, uint64 perm)
{
  if (mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
// x86_64: paging is already on since entry.S; this leaves the
// boot page table, and with it the identity map of low memory.
// cr3 takes a physical address. Writing cr3 also flushes the
// TLB, so riscv's sfence.vma before and after is not needed.
void
kvminithart()
{
  write_cr3(V2P(kernel_pagetable));
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// x86_64: 4-level paging has four levels of page-table
// pages (riscv Sv39: three). A page-table page contains 512
// 64-bit PTEs. A 64-bit virtual address is split into six
// fields:
//   48..63 -- must equal bit 47 (canonical).
//   39..47 -- 9 bits of level-3 (PML4) index.
//   30..38 -- 9 bits of level-2 (PDPT) index.
//   21..29 -- 9 bits of level-1 (PD) index.
//   12..20 -- 9 bits of level-0 (PT) index.
//    0..11 -- 12 bits of byte offset within the page.
// Kernel addresses are in the upper half, so the check is
// canonical form rather than va < MAXVA.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if (!IS_CANONICAL_ADDRESS(va))
    panic("walk");

  for (int level = 3; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    // x86_64: a PTE holds a physical address, but the kernel
    // reads page tables through the KERNBASE map, so convert
    // with P2V() and V2P() (riscv: identity map, no conversion).
    if (*pte & PTE_P) {
      pagetable = (pagetable_t)P2V(PTE2PA(*pte));
    } else {
      if (!alloc || (pagetable = (pagetable_t)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      // x86_64: the CPU ANDs the permissions of all levels, so
      // intermediate entries allow everything and the last PTE
      // decides. Never set PS (bit 7) here.
      *pte = PA2PTE(V2P(pagetable)) | PTE_P | PTE_RW | PTE_US;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  // x86_64: USERTOP (riscv: MAXVA) leaves out the guard page at
  // the top, and walk() panics on the non-canonical addresses above.
  if (va >= USERTOP)
    return 0;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    return 0;
  if ((*pte & PTE_P) == 0)
    return 0;
  if ((*pte & PTE_US) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
// x86_64: perm is uint64 (riscv: int) so that PTE_XD, bit 63,
// is not cut off.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, uint64 perm)
{
  uint64 a, last;
  pte_t *pte;

  if ((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if ((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if (size == 0)
    panic("mappages: size");

  a = va;
  last = va + size - PGSIZE;
  for (;;) {
    if ((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if (*pte & PTE_P)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_P;
    if (a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// create a user page table that maps only the kernel.
// returns 0 if out of memory.
// x86_64: riscv starts with an empty table because the trampoline
// switches satp to the kernel page table on a trap. x86_64 has no
// trampoline: a trap or system call enters the kernel with the
// user cr3 still loaded, so every user page table must map the
// kernel. Copy the PML4 entries of the kernel half (256..511) so
// that the lower-level kernel page-table pages are shared.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t)kalloc();
  if (pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  for (int i = 256; i < 512; i++) {
    pagetable[i] = kernel_pagetable[i];
  }
  return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if ((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for (a = va; a < va + npages * PGSIZE; a += PGSIZE) {
    if ((pte = walk(pagetable, a, 0)) == 0) // leaf page table entry allocated?
      continue;
    if ((*pte & PTE_P) == 0) // has physical page been allocated?
      continue;
    if (do_free) {
      // x86_64: kfree() takes the KERNBASE address of the page.
      uint64 pa = PTE2PA(*pte);
      kfree(P2V(pa));
    }
    *pte = 0;
  }
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
// x86_64: xperm is uint64 (riscv: int) so that PTE_XD survives.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, uint64 xperm)
{
  char *mem;
  uint64 a;

  if (newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for (a = oldsz; a < newsz; a += PGSIZE) {
    mem = kalloc();
    if (mem == 0) {
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    // x86_64: no PTE_R; a present page is readable. kalloc()
    // returns a KERNBASE address, so the PTE gets V2P(mem).
    if (mappages(pagetable, a, PGSIZE, V2P(mem), PTE_US | xperm) != 0) {
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if (newsz >= oldsz)
    return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
// x86_64: level is 3 for the PML4 and 0 for a page table
// (riscv: no level argument). uvmfree() starts at 3.
void
freewalk(pagetable_t pagetable, int level)
{
  // there are 2^9 = 512 PTEs in a page table.
  // x86_64: PML4 entries 256..511 are the kernel half, copied
  // from kernel_pagetable by uvmcreate(). The page-table pages
  // below them are shared with every process, so skip them.
  int n = (level == 3) ? 256 : 512;

  for (int i = 0; i < n; i++) {
    pte_t pte = pagetable[i];
    // x86_64: riscv tells a pointer to a lower-level table by
    // R/W/X being 0. On x86 an intermediate entry has the same
    // P|RW|US bits as a leaf, so the level decides instead.
    if ((pte & PTE_P) && level > 0) {
      // this PTE points to a lower-level page table.
      // x86_64: the PTE holds a physical address; P2V() to read it.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)P2V(child), level - 1);
      pagetable[i] = 0;
    } else if (pte & PTE_P) {
      panic("freewalk: leaf");
    }
  }
  kfree((void *)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if (sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  freewalk(pagetable, 3);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint64 flags; // x86_64: uint64 (riscv: uint) to keep PTE_XD
  char *mem;

  for (i = 0; i < sz; i += PGSIZE) {
    if ((pte = walk(old, i, 0)) == 0)
      continue; // page table entry hasn't been allocated
    if ((*pte & PTE_P) == 0)
      continue; // physical page hasn't been allocated
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if ((mem = kalloc()) == 0)
      goto err;
    // x86_64: read the parent's page through P2V() and map the
    // copy by its physical address.
    memmove(mem, (char *)P2V(pa), PGSIZE);
    if (mappages(new, i, PGSIZE, V2P(mem), flags) != 0) {
      kfree(mem);
      goto err;
    }
  }
  return 0;

err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_US;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 psz, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while (len > 0) {
    va0 = PGROUNDDOWN(dstva);
    if (va0 >= USERTOP) // x86_64: riscv MAXVA; see walkaddr()
      return -1;

    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) {
      if ((pa0 = vmfault(pagetable, psz, va0, 0)) == 0) {
        return -1;
      }
    }

    pte = walk(pagetable, va0, 0);
    // forbid copyout over read-only user text pages.
    if ((*pte & PTE_RW) == 0)
      return -1;

    n = PGSIZE - (dstva - va0);
    if (n > len)
      n = len;
    // x86_64: pa0 is physical; write the page through P2V().
    memmove(P2V(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, uint64 psz, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while (len > 0) {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) {
      if ((pa0 = vmfault(pagetable, psz, va0, 1)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if (n > len)
      n = len;
    // x86_64: pa0 is physical; read the page through P2V().
    memmove(dst, P2V(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, uint64 psz, char *dst, uint64 srcva,
          uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while (got_null == 0 && max > 0) {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) {
      if ((pa0 = vmfault(pagetable, psz, va0, 1)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if (n > max)
      n = max;

    // x86_64: pa0 is physical; read the page through P2V().
    char *p = (char *)P2V(pa0 + (srcva - va0));
    while (n > 0) {
      if (*p == '\0') {
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if (got_null) {
    return 0;
  } else {
    return -1;
  }
}

// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.
uint64
vmfault(pagetable_t pagetable, uint64 psz, uint64 va, int read)
{
  uint64 mem;

  if (va >= psz)
    return 0;
  va = PGROUNDDOWN(va);
  if (ismapped(pagetable, va)) {
    return 0;
  }
  mem = (uint64)kalloc();
  if (mem == 0)
    return 0;
  memset((void *)mem, 0, PGSIZE);
  // x86_64: no PTE_R; a present page is readable. sbrk memory is
  // data, so PTE_XD (riscv: no PTE_X). Return the physical address,
  // like walkaddr(), since the copy functions P2V() whichever they got.
  if (mappages(pagetable, va, PGSIZE, V2P(mem), PTE_RW | PTE_US | PTE_XD) != 0) {
    kfree((void *)mem);
    return 0;
  }
  return V2P(mem);
}

int
ismapped(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_P) {
    return 1;
  }
  return 0;
}

// TEST(6): build user page tables, copy in and out of them,
// load one into cr3, then free them and check for leaks.
// Expect "uvm_test: ok" on the serial port.
#define TEST6_SZ (2 * 1024 * 1024 + 3 * PGSIZE) // spans two PTs
#define TEST6_TOP (USERTOP - PGSIZE)            // PML4 entry 255

static int test6_fail;

static void
test6_check(int ok, char *what)
{
  if (!ok) {
    printk("uvm_test: FAIL %s\n", what);
    test6_fail = 1;
  }
}

void
uvm_test(void)
{
  char msg[] = "hello, user page table";
  char buf[sizeof(msg)];
  uint64 off = PGSIZE - 3; // crosses a page boundary
  pagetable_t upt, upt2;
  char *mem;
  int nfree;

  nfree = kfreecount();

  upt = uvmcreate();
  test6_check(upt != 0, "uvmcreate");
  test6_check(upt[511] == kernel_pagetable[511], "kernel half");
  test6_check(uvmalloc(upt, 0, TEST6_SZ, PTE_RW | PTE_XD) == TEST6_SZ,
              "uvmalloc");

  // the highest user page: the last entry freewalk() follows.
  mem = kalloc();
  test6_check(mappages(upt, TEST6_TOP, PGSIZE, V2P(mem),
                       PTE_RW | PTE_US | PTE_XD) == 0,
              "mappages top");
  test6_check(walkaddr(upt, TEST6_TOP) == V2P(mem), "walkaddr top");
  test6_check(walkaddr(upt, USERTOP) == 0, "walkaddr guard");
  test6_check(walkaddr(upt, TEST6_SZ) == 0, "walkaddr unmapped");

  test6_check(copyout(upt, TEST6_SZ, off, msg, sizeof(msg)) == 0,
              "copyout");
  memset(buf, 0, sizeof(buf));
  test6_check(copyin(upt, TEST6_SZ, buf, off, sizeof(buf)) == 0, "copyin");
  test6_check(memcmp(buf, msg, sizeof(msg)) == 0, "copyin data");
  memset(buf, 0, sizeof(buf));
  test6_check(copyinstr(upt, TEST6_SZ, buf, off, sizeof(buf)) == 0,
              "copyinstr");
  test6_check(strncmp(buf, msg, sizeof(msg)) == 0, "copyinstr data");
  test6_check(copyout(upt, TEST6_SZ, TEST6_SZ - 1, msg, 2) == -1,
              "copyout past sz");

  upt2 = uvmcreate();
  test6_check(upt2 != 0, "uvmcreate 2");
  test6_check(uvmcopy(upt, upt2, TEST6_SZ) == 0, "uvmcopy");
  test6_check(walkaddr(upt2, off) != walkaddr(upt, off), "uvmcopy pages");
  memset(buf, 0, sizeof(buf));
  test6_check(copyin(upt2, TEST6_SZ, buf, off, sizeof(buf)) == 0,
              "copyin 2");
  test6_check(memcmp(buf, msg, sizeof(msg)) == 0, "uvmcopy data");

  // run on the user page table: the kernel half must be there.
  // CR0.WP and SMAP are off, so the kernel reads the user page
  // through the user address too.
  write_cr3(V2P(upt));
  printk("uvm_test: on the user page table\n");
  test6_check(*(volatile char *)off == msg[0], "user address");
  kvminithart();

  uvmunmap(upt, TEST6_TOP, 1, 1);
  uvmfree(upt, TEST6_SZ);
  uvmfree(upt2, TEST6_SZ);
  test6_check(kfreecount() == nfree, "leak");

  if (test6_fail)
    panic("uvm_test");
  printk("uvm_test: ok\n");
}
