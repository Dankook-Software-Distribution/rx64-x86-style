// x86_64: the I/O APIC routes device IRQ pins to a CPU's local
// APIC (riscv: the PLIC). ioapicinit() points pin n at vector
// T_IRQ0 + n, masked; a driver unmasks its pin with
// ioapicenable(). No claim/complete: the EOI goes to the LAPIC.

#include "types.h"
#include "x86.h"
#include "memlayout.h"
#include "defs.h"
#include "traps.h"

// IOAPIC registers (Intel 82093AA datasheet). Only two words are
// memory-mapped: write a register index to the select word, then
// read or write that register through the window word.
// Byte offsets from DEV2V(IOAPIC_PA):
#define IO_APIC_REGSEL 0x00  // register select (IOREGSEL)
#define IO_APIC_WIN    0x10  // data window (IOWIN)
// Register indices, written to IO_APIC_REGSEL:
#define IO_APIC_VER    0x01  // version; bits 16-23 = max redirection entry
#define IO_APIC_REDTBL 0x10  // pin n: low half 0x10+2*n, high half 0x11+2*n
#define   IO_APIC_REDIR_MASKED (1 << 16)  // in the low half

static uint32
ioapic_read(uint32 reg)
{
  *(volatile uint32 *)(DEV2V(IOAPIC_PA) + IO_APIC_REGSEL) = reg;
  return *(volatile uint32 *)(DEV2V(IOAPIC_PA) + IO_APIC_WIN);
}

static void
ioapic_write(uint32 reg, uint32 data)
{
  *(volatile uint32 *)(DEV2V(IOAPIC_PA) + IO_APIC_REGSEL) = reg;
  *(volatile uint32 *)(DEV2V(IOAPIC_PA) + IO_APIC_WIN) = data;
}

void
ioapicinit(void)
{
  // Bits 16-23 of VER hold the index of the last redirection
  // entry (entries - 1), so the loop includes it.
  uint32 maxredir = (ioapic_read(IO_APIC_VER) >> 16) & 0xff;

  for (int i = 0; i <= maxredir; ++i) {
    ioapic_write(IO_APIC_REDTBL + 2 * i, IO_APIC_REDIR_MASKED | (T_IRQ0 + i));
    ioapic_write(IO_APIC_REDTBL + 2 * i + 1, 0);
  }
}

// Route irq to vector T_IRQ0 + irq on cpu, edge-triggered and
// active high as ISA devices are, and unmask it. The destination
// goes in first so the pin is never live with a stale one.
// cpu is the target's local APIC ID (0 with -smp 1; see step 8).
void
ioapicenable(int irq, int cpu)
{
  ioapic_write(IO_APIC_REDTBL + 2 * irq + 1, cpu << 24);
  ioapic_write(IO_APIC_REDTBL + 2 * irq, T_IRQ0 + irq);
}
