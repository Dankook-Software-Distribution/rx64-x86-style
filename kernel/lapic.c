// x86_64: the local APIC, one per CPU (riscv: CLINT for the
// timer and PLIC for claim/complete). It runs the timer at
// T_IRQ0 + IRQ_TIMER and takes an EOI after every interrupt.

#include "types.h"
#include "x86.h"
#include "memlayout.h"
#include "defs.h"
#include "traps.h"

// Local APIC registers, as byte offsets from the LAPIC base
// (Intel SDM Vol. 3A Table 11-1). Names and values follow Linux's
// asm/apicdef.h; only the ones the kernel uses are here.
#define APIC_LVR      0x30  // version
#define APIC_TASKPRI  0x80  // task priority
#define APIC_EOI      0xB0  // end of interrupt
#define APIC_SPIV     0xF0  // spurious interrupt vector
#define   APIC_SPIV_APIC_ENABLED  (1 << 8)
#define APIC_LVTT     0x320 // LVT timer
#define   APIC_LVT_TIMER_PERIODIC (1 << 17)
#define   APIC_LVT_MASKED         (1 << 16) // in every LVT entry
#define APIC_LVT0     0x350 // LVT LINT0
#define APIC_LVT1     0x360 // LVT LINT1
#define APIC_TMICT    0x380 // timer initial count
#define APIC_TMCCT    0x390 // timer current count
#define APIC_TDCR     0x3E0 // timer divide configuration
#define   APIC_TDR_DIV_1          0xB

// LAPIC registers are 32-bit MMIO in the page kvmmake() maps at
// DEV2V(LAPIC_PA). off is a byte offset, so add it before the
// cast; volatile makes every access reach the device.
static uint32
lapic_read(uint32 off)
{
  return *(volatile uint32 *)(DEV2V(LAPIC_PA) + off);
}

static void
lapic_write(uint32 off, uint32 val)
{
  volatile uint32 *reg = (volatile uint32 *)(DEV2V(LAPIC_PA) + off);
  *reg = val;
}

// enable this CPU's LAPIC and start its timer.
// Writing the initial count starts the timer, so it goes last.
// 10000000 is an uncalibrated guess (~100Hz on qemu).
void
lapicinit(void)
{
  lapic_write(APIC_SPIV, APIC_SPIV_APIC_ENABLED | T_SPURIOUS);
  lapic_write(APIC_TDCR, APIC_TDR_DIV_1);
  lapic_write(APIC_LVTT, APIC_LVT_TIMER_PERIODIC | (T_IRQ0 + IRQ_TIMER));
  lapic_write(APIC_TMICT, 10000000);

  // mask the virtual-wire path from the 8259s.
  lapic_write(APIC_LVT0, APIC_LVT_MASKED);
  lapic_write(APIC_LVT1, APIC_LVT_MASKED);

  // accept interrupts of every priority.
  lapic_write(APIC_TASKPRI, 0);

  // keeps lapic_read() in use (-Werror=unused-function) until
  // something reads a LAPIC register for real.
  lapic_read(APIC_LVTT);
}

// acknowledge the current interrupt. Without it the LAPIC
// delivers nothing of equal or lower priority again.
void
lapiceoi(void)
{
  lapic_write(APIC_EOI, 0);
}

