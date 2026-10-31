// x86_64: the interrupt descriptor table (riscv: stvec held one
// handler address for every trap). The IDT has one gate per
// vector, 256 in all, and each gate points at that vector's stub
// in vectors.S. All stubs then go through alltraps to trap().

#include "types.h"
#include "x86.h"
#include "traps.h"

// a 64-bit IDT gate, 16 bytes (SDM Vol. 3A Figure 7-8). The
// handler address is split into three pieces around the other
// fields. Field names follow Linux's struct gate_struct.
struct gate_struct {
  uint16 offset_low;    // handler address bits 0..15
  uint16 segment;       // code segment selector for the handler
  uint8  ist;           // interrupt stack table index; 0 = none
  uint8  attr;          // type (bits 0..3), DPL (5..6), P (7)
  uint16 offset_middle; // handler address bits 16..31
  uint32 offset_high;   // handler address bits 32..63
  uint32 reserved;
};
_Static_assert(sizeof(struct gate_struct) == 16, "IDT gate must be 16 bytes");

// one IDT shared by all CPUs. Each CPU still loads it with lidt.
static struct gate_struct idt[256];

// handler addresses, from vectors.S.
extern uint64 vectors[];

// fill in every gate and load the IDT.
// attr 0x8E is P = 1, DPL 0 and type 0xE, a 64-bit interrupt
// gate, which clears IF on entry; a trap gate (0xF) would leave
// interrupts on. Linux also uses interrupt gates for nearly all
// vectors. Only T_SYSCALL has DPL 3, so that user code can
// reach it with int; trap() turns interrupts back on for it.
void
idtinit(void)
{
  for (int i = 0; i < 256; i++) {
    uint64 addr = vectors[i];

    idt[i].offset_low    = addr & 0xffff;
    idt[i].segment       = KCSEG;
    idt[i].ist           = 0;
    idt[i].attr          = 0x8e;
    idt[i].offset_middle = (addr >> 16) & 0xffff;
    idt[i].offset_high   = (addr >> 32) & 0xffffffff;
  }

  idt[T_SYSCALL].attr = 0xee; // P = 1, DPL 3, interrupt gate

  lidt(idt, sizeof(idt));
}
