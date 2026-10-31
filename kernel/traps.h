// x86_64: trap numbers (riscv: scause codes in riscv.h).
// Vectors 0-31 are CPU exceptions, named as in the SDM. "err"
// marks those with an error code; vectors.S pushes 0 for the
// rest. Only #defines here: vectors.S includes this file too.

#define T_DE     0  // divide error
#define T_DB     1  // debug
#define T_NMI    2  // non-maskable interrupt
#define T_BP     3  // breakpoint (int3)
#define T_OF     4  // overflow (into)
#define T_BR     5  // bound range exceeded
#define T_UD     6  // invalid opcode
#define T_NM     7  // device not available
#define T_DF     8  // double fault, err (always 0)
//                  9     reserved (coprocessor segment overrun)
#define T_TS    10  // invalid TSS, err
#define T_NP    11  // segment not present, err
#define T_SS    12  // stack-segment fault, err
#define T_GP    13  // general protection, err
#define T_PF    14  // page fault, err (address in cr2)
//                  15    reserved
#define T_MF    16  // x87 floating-point error
#define T_AC    17  // alignment check, err (always 0)
#define T_MC    18  // machine check
#define T_XM    19  // SIMD floating-point exception
#define T_VE    20  // virtualization exception
#define T_CP    21  // control protection, err
//                  22-31 reserved

// Vectors 32-255 are free for software. Device interrupts start
// at T_IRQ0; IRQ n arrives as vector T_IRQ0 + n.
#define T_IRQ0      32
#define T_SYSCALL   64  // system call, int $64 (riscv: ecall)

#define IRQ_TIMER    0  // LAPIC timer
#define IRQ_COM1     4  // serial port, through the IOAPIC
#define IRQ_IDE     14  // primary IDE disk, through the IOAPIC

// LAPIC spurious-interrupt vector, as Linux's SPURIOUS_APIC_VECTOR.
#define T_SPURIOUS 0xFF
