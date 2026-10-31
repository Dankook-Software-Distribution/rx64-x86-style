// x86_64: the interrupt descriptor table (riscv: stvec held one
// handler for every trap). Each of the 256 gates points at its
// stub in vectors.S, which goes through alltraps to trap().

#include "types.h"
#include "x86.h"
#include "traps.h"

// a 64-bit IDT gate, 16 bytes. The handler address is split into
// three pieces around the other fields.
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
// attr 0x8E is P = 1, DPL 0, 64-bit interrupt gate (clears IF).
// Only T_SYSCALL has DPL 3, so that user code can use int.
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
