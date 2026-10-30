// x86_64 replacement for riscv.h.
// Lines tagged "x86_64:" differ from xv6-riscv; see
// Intel SDM Vol. 3A Figure 5-11 for the paging-entry formats.

// x86_64: rflags bits. riscv used sstatus.SIE for this.
#define FL_IF 0x200 // interrupt enable

// x86_64: control register and EFER bits used by entry.S to turn
// on paging and long mode (riscv: satp mode field). See SDM
// Vol. 3A 2.5 and 10.8.5 for the enable sequence.
#define CR0_PG   (1L << 31) // paging
#define CR4_PAE  (1L << 5)  // physical address extension; required by long mode

#define MSR_EFER 0xC0000080 // extended feature enable register
#define EFER_LME (1L << 8)  // long mode enable
#define EFER_NXE (1L << 11) // makes PTE_XD usable; reserved (#PF) otherwise

// x86_64: segment selectors
// selector = GDT index * 8; low 3 bits are TI and RPL, 0 here.
// users add RPL 3 themselves (UCSEG | 3, UDSEG | 3).
// the order is fixed by syscall/sysret, as in Linux: syscall
// loads CS = STAR[47:32] and SS = that + 8, sysret loads
// SS = STAR[63:48] + 8 and CS = STAR[63:48] + 16. so with
// STAR[47:32] = KCSEG and STAR[63:48] = KDSEG, kernel data must
// follow kernel code and user code must follow user data.
// Linux's 32-bit compat slots are left out; there is no
// 32-bit user mode.
#define KCSEG (1 << 3) // kernel 64-bit code, GDT index 1
#define KDSEG (2 << 3) // kernel data, GDT index 2 (syscall's SS)
#define UDSEG (3 << 3) // user data, GDT index 3
#define UCSEG (4 << 3) // user 64-bit code, GDT index 4
#define TSSSEG (5 << 3) // TSS, GDT index 5 and 6 (16 bytes)
#define NSEGS 7        // null, 4 segments above, TSS (16 bytes, 2 slots)

#ifndef __ASSEMBLER__

// enable device interrupts
// x86_64: sti sets rflags.IF (riscv: set sstatus.SIE).
// "memory" keeps the compiler from moving loads/stores across it.
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
// x86_64: devices like COM1 live in a separate I/O port space,
// reached only with in/out instructions (riscv: the UART was
// memory-mapped, so plain loads and stores worked).
// outb takes the byte in al and the port in dx or as an 8-bit
// immediate: "a" puts data in al, "Nd" puts port in dx or an
// immediate. both are inputs only, so there are no outputs.
static inline void
outb(uint16 port, uint8 data)
{
  asm volatile("outb %0, %1" : : "a"(data), "Nd"(port));
}

// load the GDT register
// x86_64: riscv has no segmentation, so nothing corresponds.
// lgdt reads a 10-byte operand from memory: a 2-byte limit
// (size - 1) and an 8-byte base, the GDT's virtual address.
// packed drops the 6 bytes of padding the compiler would put
// after limit to align base; without it lgdt reads a bad base.
// "m" passes desc itself as a memory operand.
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
// x86_64: riscv has nothing like it. ltr looks up sel in the
// current GDT, loads TR with the TSS base and limit from that
// descriptor, and marks the descriptor busy (type 9 -> 0xB), so a
// second ltr with the same descriptor raises #GP. %w0 names the
// 16-bit register, since ltr takes r/m16.
static inline void
ltr(uint16 sel)
{
  asm volatile("ltr %w0" : : "r"(sel));
}

// load the interrupt descriptor table register
// x86_64: replaces riscv's write to stvec. The operand has the
// same 10-byte form as lgdt's: limit (size - 1) and the IDT's
// virtual address. idt is void * because struct gate_struct is
// private to idt.c.
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
// x86_64: on a page fault the CPU puts the virtual address it
// failed to access in cr2 (riscv: stval). cr2 is not saved in
// the trapframe, and a later page fault overwrites it.
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
// x86_64: riscv has nothing like it; uservec loaded the kernel
// stack pointer from the trapframe itself. On an interrupt or
// trap from CPL 3 the CPU loads rsp from sp0 before pushing the
// trap frame. 64-bit mode does no hardware task switching, so
// this is all the TSS is used for: sp1, sp2 and ist are unused
// (no IST stacks), and io_bitmap_base beyond the TSS limit means
// there is no I/O permission bitmap. packed keeps sp0 at offset
// 4, where the compiler would otherwise pad it to 8.
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
// x86_64: riscv's wfi wakes on a pending interrupt even with
// sstatus.SIE clear, but hlt with IF=0 never wakes. sti delays
// interrupts until the next instruction ends, so sti; hlt
// cannot lose an interrupt in between. Keep both in one asm so
// the compiler cannot put anything there. Linux: native_safe_halt().
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

// x86_64: riscv had V/R/W/X/U. x86 has no R bit (present implies
// readable) and X is inverted into XD, which needs EFER.NXE=1.
// Bits 6..8 differ by level (D/PS/G/PAT); xv6 leaves them 0.
// Bit 7 must stay 0 in PDPTEs and PDEs, or the CPU treats the
// entry as a 1GB/2MB page instead of a pointer to a table. Only
// entry.S's boot page table sets it (PTE_PS), for 2MB pages.
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
// x86_64: 48-bit addresses must be canonical (bits 63..47 equal),
// so the lower half ends at 1<<47; 0x0000800000000000 is
// non-canonical and faults with #GP.
#define MAXVA (1L << (9 + 9 + 9 + 9 + 12 - 1))

// x86_64: a virtual address is canonical when bits 63..47 are all
// equal, i.e. va is bit 47 sign-extended (riscv Sv39 instead
// required bits 63..39 to be zero for walk()). Using a
// non-canonical address faults with #GP, and PX() ignores bits
// 63..48, so walk() would map such an address at the wrong place.
// CANONICAL_ADDRESS() sign-extends bit (bits - 1): shift left as
// unsigned, then right as signed so the top fills with that bit.
// Names follow Linux's __canonical_address() and
// __is_canonical_address().
#define CANONICAL_ADDRESS(va, bits) \
  ((uint64)((int64)((uint64)(va) << (64 - (bits))) >> (64 - (bits))))
#define IS_CANONICAL_ADDRESS(va) (CANONICAL_ADDRESS(va, 48) == (uint64)(va))
