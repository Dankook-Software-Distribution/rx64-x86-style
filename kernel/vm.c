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

  // x86_64: RAM is at KERNBASE + pa (riscv: identity map). COM1
  // is an I/O port; the virtio disk's BARs go in later, with ioremap().

  // IOAPIC and LAPIC registers, one page each.
  // x86_64: these replace the PLIC. They sit above 2GB physical,
  // so they go at DEV2V(pa), uncached (PCD | PWT).
  kvmmap(kpgtbl, DEV2V(IOAPIC_PA), IOAPIC_PA, PGSIZE,
         PTE_RW | PTE_XD | PTE_PCD | PTE_PWT);
  kvmmap(kpgtbl, DEV2V(LAPIC_PA), LAPIC_PA, PGSIZE,
         PTE_RW | PTE_XD | PTE_PCD | PTE_PWT);

  // map kernel text executable and read-only.
  // x86_64: perm 0 (riscv: PTE_R | PTE_X); no R bit, and no XD
  // means executable.
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

// x86_64: map device registers at DEV2V(pa), uncached, and
// return their address, as Linux's ioremap(). For PCI BARs, which
// the firmware places at boot. Pages already mapped are kept.
void *
ioremap(uint64 pa, uint64 size)
{
  if (pa + size > 0x100000000UL)
    panic("ioremap: above 4GB");
  for (uint64 a = PGROUNDDOWN(pa); a < pa + size; a += PGSIZE) {
    pte_t *pte = walk(kernel_pagetable, DEV2V(a), 1);
    if (pte == 0)
      panic("ioremap");
    if ((*pte & PTE_P) == 0)
      *pte = PA2PTE(a) | PTE_RW | PTE_XD | PTE_PCD | PTE_PWT | PTE_P;
  }
  return (void *)DEV2V(pa);
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
// boot page table. Writing cr3 flushes the TLB (riscv: sfence.vma).
void
kvminithart()
{
  write_cr3(V2P(kernel_pagetable));
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// x86_64: four levels (riscv Sv39: three). A virtual address is:
//   48..63 -- must equal bit 47 (canonical).
//   39..47 -- 9 bits of level-3 (PML4) index.
//   30..38 -- 9 bits of level-2 (PDPT) index.
//   21..29 -- 9 bits of level-1 (PD) index.
//   12..20 -- 9 bits of level-0 (PT) index.
//    0..11 -- 12 bits of byte offset within the page.
// The kernel is in the upper half, so check canonical form, not MAXVA.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if (!IS_CANONICAL_ADDRESS(va))
    panic("walk");

  for (int level = 3; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    // x86_64: PTEs hold physical addresses; P2V()/V2P() convert.
    if (*pte & PTE_P) {
      pagetable = (pagetable_t)P2V(PTE2PA(*pte));
    } else {
      if (!alloc || (pagetable = (pagetable_t)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      // x86_64: the CPU ANDs all levels, so the leaf PTE decides.
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

  // x86_64: USERTOP (riscv: MAXVA); walk() panics above it.
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
// x86_64: perm is uint64 (riscv: int) to keep PTE_XD, bit 63.
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
// x86_64: a trap keeps the user cr3 (riscv: the trampoline switched
// satp), so share the kernel half, PML4 entries 256..511.
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

// x86_64: how many bytes from the start of va's hole a missing
// page table leaves unmapped: 2MB, 1GB or 512GB. Loops over a
// user range use it to skip holes, since a 47-bit space is too
// big to step through a page at a time (riscv Sv39 was 256GB).
static uint64
holesize(pagetable_t pagetable, uint64 va)
{
  for (int level = 3; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if ((*pte & PTE_P) == 0)
      return 1L << PXSHIFT(level);
    pagetable = (pagetable_t)P2V(PTE2PA(*pte));
  }
  return PGSIZE;
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
    if ((pte = walk(pagetable, a, 0)) == 0) { // leaf page table entry allocated?
      // x86_64: skip the rest of the hole.
      uint64 n = holesize(pagetable, a);
      a = (a & ~(n - 1)) + n - PGSIZE;
      continue;
    }
    if ((*pte & PTE_P) == 0) // has physical page been allocated?
      continue;
    if (do_free) {
      // x86_64: kfree() takes a virtual address.
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
    // x86_64: no PTE_R (riscv); the PTE gets V2P(mem).
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
// x86_64: level is 3 for the PML4, 0 for a page table.
void
freewalk(pagetable_t pagetable, int level)
{
  // there are 2^9 = 512 PTEs in a page table.
  // x86_64: skip PML4 entries 256..511, the shared kernel half.
  int n = (level == 3) ? 256 : 512;

  for (int i = 0; i < n; i++) {
    pte_t pte = pagetable[i];
    // x86_64: the level tells a table from a leaf (riscv: R/W/X
    // bits); the flags look the same here.
    if ((pte & PTE_P) && level > 0) {
      // this PTE points to a lower-level page table.
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
    if ((pte = walk(old, i, 0)) == 0) {
      // page table entry hasn't been allocated.
      // x86_64: skip the rest of the hole.
      uint64 n = holesize(old, i);
      i = (i & ~(n - 1)) + n - PGSIZE;
      continue;
    }
    if ((*pte & PTE_P) == 0)
      continue; // physical page hasn't been allocated
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if ((mem = kalloc()) == 0)
      goto err;
    // x86_64: PTEs hold physical addresses.
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
  // x86_64: no PTE_R; PTE_XD for data. Returns a physical
  // address, like walkaddr().
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
