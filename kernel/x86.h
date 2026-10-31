// x86_64 replacement for riscv.h.
// Lines tagged "x86_64:" differ from xv6-riscv; see
// Intel SDM Vol. 3A Figure 5-11 for the paging-entry formats.

// x86_64: rflags bits. riscv used sstatus.SIE for this.
#define FL_IF 0x200 // interrupt enable

// x86_64: control register and EFER bits used by entry.S to turn
// on paging and long mode (riscv: satp mode field).
#define CR0_PG   (1L << 31) // paging
#define CR4_PAE  (1L << 5)  // physical address extension; required by long mode

#define MSR_EFER 0xC0000080 // extended feature enable register
#define EFER_LME (1L << 8)  // long mode enable
#define EFER_NXE (1L << 11) // makes PTE_XD usable; reserved (#PF) otherwise

// x86_64: segment selectors (riscv has no segmentation).
// selector = GDT index * 8; users add RPL 3 (UCSEG | 3).
// syscall/sysret fix the order, as in Linux: kernel data after
// kernel code, user code after user data.
#define KCSEG (1 << 3) // kernel 64-bit code, GDT index 1
#define KDSEG (2 << 3) // kernel data, GDT index 2 (syscall's SS)
#define UDSEG (3 << 3) // user data, GDT index 3
#define UCSEG (4 << 3) // user 64-bit code, GDT index 4
#define TSSSEG (5 << 3) // TSS, GDT index 5 and 6 (16 bytes)
#define NSEGS 7        // null, 4 segments above, TSS (16 bytes, 2 slots)

#ifndef __ASSEMBLER__

// enable device interrupts
// x86_64: sti sets rflags.IF (riscv: set sstatus.SIE).
static inline void
intr_on()
{
  asm volatile("sti" ::: "memory");
}

// disable device interrupts
// x86_64: cli clears rflags.IF (riscv: clear sstatus.SIE).
static inline void
intr_off()
{
  asm volatile("cli" ::: "memory");
}

// are device interrupts enabled?
// x86_64: rflags has no mov form, so read it through the stack.
static inline int
intr_get()
{
  uint64 rflags;
  asm volatile("pushfq; popq %0" : "=r"(rflags));
  return (rflags & FL_IF) != 0;
}

// x86_64: rsp instead of riscv sp.
static inline uint64
r_sp()
{
  uint64 x;
  asm volatile("mov %%rsp, %0" : "=r"(x));
  return x;
}

// write a byte to an I/O port
// x86_64: COM1 and IDE are in the I/O port space, reached only
// with in/out (riscv: the UART was memory-mapped).
static inline void
outb(uint16 port, uint8 data)
{
  asm volatile("outb %0, %1" : : "a"(data), "Nd"(port));
}

// load the GDT register
// x86_64: riscv has no segmentation. lgdt reads a 10-byte
// limit-and-base operand, hence packed.
static inline void
lgdt(uint64 *gdt, int size)
{
  struct gdtdesc {
    uint16 limit;
    uint64 base;
  } __attribute__((packed));

  struct gdtdesc desc = { size - 1, (uint64)gdt };

  asm volatile("lgdt %0" : : "m"(desc));
}

// load the task register
// x86_64: riscv has nothing like it. ltr marks the descriptor
// busy, so a second ltr with it raises #GP.
static inline void
ltr(uint16 sel)
{
  asm volatile("ltr %w0" : : "r"(sel));
}

// load the interrupt descriptor table register
// x86_64: replaces riscv's write to stvec. Same operand as lgdt.
static inline void
lidt(void *idt, int size)
{
  struct idtdesc {
    uint16 limit;
    uint64 base;
  } __attribute__((packed));

  struct idtdesc desc = { size - 1, (uint64)idt };

  asm volatile("lidt %0" : : "m"(desc));
}

// read the page-fault address
// x86_64: riscv: stval. Not in the trapframe; read it early.
static inline uint64
read_cr2()
{
  uint64 x;
  asm volatile("mov %%cr2, %0" : "=r"(x));
  return x;
}

static inline void
write_cr3(uint64 pa)
{
  asm volatile("mov %0, %%cr3" : : "r"(pa) : "memory");
}

typedef uint64 pte_t;
typedef uint64 *pagetable_t; // 512 PTEs

// 64-bit task state segment (SDM Vol. 3A Figure 10-11), named
// after Linux's struct x86_hw_tss.
// x86_64: riscv has nothing like it (uservec read the stack from
// the trapframe). A trap from CPL 3 loads rsp from sp0; nothing
// else here is used. packed keeps sp0 at offset 4.
struct x86_hw_tss {
  uint32 reserved1;
  uint64 sp0;
  uint64 sp1;
  uint64 sp2;
  uint64 reserved2;
  uint64 ist[7];
  uint64 reserved3;
  uint16 reserved4;
  uint16 io_bitmap_base;
} __attribute__((packed));
_Static_assert(sizeof(struct x86_hw_tss) == 104,
               "x86_hw_tss layout must match the 64-bit TSS");
_Static_assert(__builtin_offsetof(struct x86_hw_tss, sp0) == 4,
               "the CPU reads sp0 at offset 4");

// enable interrupts and wait for one
// x86_64: hlt with IF=0 never wakes (riscv's wfi does). sti takes
// effect after the next instruction, so nothing slips in between.
static inline void
safe_halt()
{
  asm volatile("sti; hlt" : : : "memory");
}

#endif // __ASSEMBLER__

#define PGSIZE  4096 // bytes per page
#define PGSHIFT 12   // bits of offset within a page
// x86_64: physical-address field of a paging-structure entry,
// bits 12..51. Same position at all four levels when PS=0.
#define PGMASK  0x000FFFFFFFFFF000

#define PGROUNDUP(sz)  (((sz) + PGSIZE - 1) & ~(PGSIZE - 1))
#define PGROUNDDOWN(a) (((a)) & ~(PGSIZE - 1))

// x86_64: riscv had V/R/W/X/U. x86 has no R bit (present means
// readable), and XD (with EFER.NXE) replaces X. Only entry.S
// sets PS, for its 2MB boot pages.
#define PTE_P   (1L << 0)  // present
#define PTE_RW  (1L << 1)  // Read/write
#define PTE_US  (1L << 2)  // User/supervisor
#define PTE_PWT (1L << 3)  // Page-level write-through
#define PTE_PCD (1L << 4)  // Page-level cache disable
#define PTE_A   (1L << 5)  // Accessed
#define PTE_PS  (1L << 7)  // page size: 2MB page in a PDE (boot only)
#define PTE_XD  (1L << 63) // execute-disable

// x86_64: the address field starts at bit 12, the same place as in
// the physical address, so no shift is needed (riscv: >>12 <<10).
#define PA2PTE(pa) ((uint64)(pa) & PGMASK)

#define PTE2PA(pte) ((pte) & PGMASK)

// x86_64: keep XD (bit 63) so uvmcopy() preserves no-execute.
#define PTE_FLAGS(pte) ((pte) & 0x8000000000000FFF)

// x86_64: four 9-bit page table indices (riscv Sv39: three).
// level 3 = PML4, 2 = PDPT, 1 = PD, 0 = PT.
#define PXMASK         0x1FF // 9 bits
#define PXSHIFT(level) (PGSHIFT + (9 * (level)))
#define PX(level, va)  ((((uint64)(va)) >> PXSHIFT(level)) & PXMASK)

// one beyond the highest possible user virtual address.
// x86_64: the lower canonical half ends at 1<<47.
#define MAXVA (1L << (9 + 9 + 9 + 9 + 12 - 1))

// x86_64: canonical means bits 63..47 are equal (riscv Sv39:
// 63..39 zero). PX() ignores bits 63..48, so walk() checks this.
// CANONICAL_ADDRESS() sign-extends bit (bits - 1).
#define CANONICAL_ADDRESS(va, bits) \
  ((uint64)((int64)((uint64)(va) << (64 - (bits))) >> (64 - (bits))))
#define IS_CANONICAL_ADDRESS(va) (CANONICAL_ADDRESS(va, 48) == (uint64)(va))
