// Memory layout
//
// x86_64: unlike xv6-riscv, the kernel is not identity-mapped, so
// physical and virtual addresses differ. This file has four parts:
// physical addresses, physical <-> virtual conversion, kernel
// virtual addresses, and user virtual addresses.
// Needs PGSIZE and MAXVA from x86.h; include x86.h first.

// ---------------------------------------------------------------
// Physical addresses (qemu -machine pc)
//
// 00000000 -- low RAM; entryother is copied to 0x7000 to boot APs
// 000A0000 -- VGA and BIOS ROM; the MP table is near 0xF0000
// 00100000 -- multiboot loads the kernel here (EXTMEM)
// end      -- start of kernel page allocation area
// PHYSTOP  -- end of RAM used by the kernel
// FEC00000 -- IOAPIC
// FEE00000 -- LAPIC
//
// Never dereference these directly; convert with P2V or DEV2V.
// ---------------------------------------------------------------

#define EXTMEM    0x100000   // kernel load address
// x86_64: a physical address now (riscv: KERNBASE + 128MB).
// Must stay below 2GB, the size of the KERNBASE window.
#define PHYSTOP   0x8000000  // 128MB

// x86_64: IOAPIC and LAPIC replace PLIC and CLINT.
#define IOAPIC_PA 0xFEC00000
#define LAPIC_PA  0xFEE00000

// x86_64: I/O ports, not MMIO (riscv: UART0, VIRTIO0).
#define COM1      0x3F8
#define IDE_BASE  0x1F0
#define IDE_CTL   0x3F6 // device control; bit 1 (nIEN) masks IRQ 14

// ---------------------------------------------------------------
// Physical <-> kernel virtual conversion
//
// x86_64: higher-half kernel. All of physical RAM is mapped at
// KERNBASE + pa, inside the top 2GB required by -mcmodel=kernel.
// ---------------------------------------------------------------

// x86_64: a virtual address now (riscv: start of RAM).
#define KERNBASE  0xFFFFFFFF80000000 // first kernel virtual address
#define KERNLINK  (KERNBASE + EXTMEM) // address where kernel is linked

// Valid only inside the KERNBASE window (kernel image, kalloc
// pages, page tables). Not for KSTACK or device addresses.
#ifndef __ASSEMBLER__
#define V2P(a) ((uint64)(a) - KERNBASE)
#define P2V(a) ((void *)((char *)(a) + KERNBASE))
#endif

// Same without casts, for assembly and integer constants.
#define V2P_WO(x) ((x) - KERNBASE)
#define P2V_WO(x) ((x) + KERNBASE)

// x86_64: device MMIO is above 2GB physical, out of reach of
// P2V, so it gets its own window at DEVBASE + pa.
#define DEVBASE   0xFFFFFFFF00000000
#define DEV2V(pa) (DEVBASE + (pa))

// ---------------------------------------------------------------
// Kernel virtual addresses
//
// KERNBASE            -- P2V(0)
// KERNLINK            -- kernel text and data
// P2V(PHYSTOP)        -- KSTACKBASE
// KSTACKBASE + 512KB  -- end of kernel stacks
// DEV2V(IOAPIC_PA)    -- IOAPIC
// DEV2V(LAPIC_PA)     -- LAPIC
// ---------------------------------------------------------------

// x86_64: kernel stacks start just above the RAM mapping
// (riscv: beneath TRAMPOLINE), each with a guard page below.
#define KSTACKBASE P2V_WO(PHYSTOP)
#define KSTACK(p)  (KSTACKBASE + ((p) * 2 + 1) * PGSIZE)

// ---------------------------------------------------------------
// User virtual addresses
//
// Address zero first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
//   ...
//   USERTOP
//
// x86_64: no TRAMPOLINE or TRAPFRAME pages in user space;
// the trapframe sits at the top of the kernel stack.
// ---------------------------------------------------------------

// x86_64: replaces TRAPFRAME as the user upper bound. The page
// below MAXVA stays unmapped, for a later sysret.
#define USERTOP (MAXVA - PGSIZE)
