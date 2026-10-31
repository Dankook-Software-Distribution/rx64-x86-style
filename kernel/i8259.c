// x86_64: the legacy 8259A interrupt controllers (riscv has
// none). The BIOS leaves IRQ 0-7 on vectors 8-15, on top of CPU
// exceptions, and the kernel uses the APICs instead, so the only
// job here is to mask them all.

#include "types.h"
#include "x86.h"

// interrupt mask registers (I/O ports); a 1 bit masks that IRQ.
// Names follow Linux's asm/i8259.h.
#define PIC_MASTER_IMR 0x21
#define PIC_SLAVE_IMR  0xA1

// mask all IRQs on both 8259s. Requests still latch in the IRR,
// but none reaches the CPU. Called before interrupts are enabled.
void
picinit(void)
{
  outb(PIC_MASTER_IMR, 0xFF);
  outb(PIC_SLAVE_IMR, 0xFF);
}
