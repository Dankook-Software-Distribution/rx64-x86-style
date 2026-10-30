// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.
//
// x86_64: pages are handed out and taken back as kernel virtual
// addresses in the KERNBASE window (riscv: RAM was identity-
// mapped, so a page's physical and virtual addresses were the
// same). Despite the name, the pa arguments below are such
// addresses; callers use V2P() to put a page in a PTE or CR3.

#include "types.h"
#include "param.h"
#include "x86.h"
#include "memlayout.h"
#include "spinlock.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

// x86_64: PHYSTOP is a physical address now, so convert it to
// match end, which kernel.ld places at a high address. entry.S's
// boot page table maps the first 1GB at KERNBASE, which covers
// this range, so kalloc works before kvminit() runs.
void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void *)P2V(PHYSTOP));
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  // x86_64: the same range as kinit(), in virtual addresses.
  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end ||
      (char *)pa >= (char *)P2V(PHYSTOP))
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run *)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if (r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if (r)
    memset((char *)r, 5, PGSIZE); // fill with junk
  return (void *)r;
}

// TEST(6): count the free pages, so that uvm_test() can check
// that uvmfree() gives back every page it took.
int
kfreecount(void)
{
  struct run *r;
  int n = 0;

  acquire(&kmem.lock);
  for (r = kmem.freelist; r; r = r->next)
    n++;
  release(&kmem.lock);
  return n;
}
