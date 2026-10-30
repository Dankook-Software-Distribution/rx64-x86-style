#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "x86.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "traps.h"

struct spinlock tickslock;
uint ticks;

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// x86_64: the local APIC timer interrupt (riscv: clockintr()).
// Count a tick and then send an EOI to the local APIC; without
// it the APIC keeps the vector in service and delivers no
// further interrupts of equal or lower priority. (riscv: writing
// stimecmp both cleared the request and armed the next one; the
// LAPIC timer is periodic and rearms itself.)
static void
timerintr(void)
{
  if (cpuid() == 0) {
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks);
    release(&tickslock);
  }
  lapiceoi();
}

// x86_64: print an unexpected trap, named after Linux's
// show_regs(). It only prints; the caller decides what to do
// next, since a kernel fault will panic but a user fault will
// only kill the process (riscv: kerneltrap() and usertrap()
// each printed scause, sepc and stval themselves).
// cs tells who trapped: its low 2 bits are the CPL (0 kernel,
// 3 user). rsp and rflags are those at the time of the trap.
// Uses earlyputs/earlyputx rather than printk, so a fault
// while printk holds its lock does not deadlock here.
static void
show_regs(struct trapframe *tf)
{
  earlyputs("trap ");
  earlyputx(tf->trapno);
  earlyputs(" err ");
  earlyputx(tf->err);
  if (tf->trapno == T_PF) {
    earlyputs(" cr2 ");
    earlyputx(read_cr2());
  }
  earlyputs("\nrip ");
  earlyputx(tf->rip);
  earlyputs(" cs ");
  earlyputx(tf->cs);
  earlyputs(" rsp ");
  earlyputx(tf->rsp);
  earlyputs(" rflags ");
  earlyputx(tf->rflags);
  earlyputs("\n");
}

// x86_64: every interrupt, exception and system call comes here,
// from the kernel or from user space alike (riscv: usertrap() via
// trampoline.S and kerneltrap() via kernelvec.S). alltraps in
// trapasm.S builds the trapframe on the kernel stack and passes
// it in tf; returning goes back through trapret and iretq.
// The vector number in tf->trapno tells what happened, so one
// switch replaces the scause checks of riscv's devintr().
// Any other trap is still a kernel bug for now, so print it and
// stop, as kerneltrap() panics on an unknown trap. Returning
// would rerun a faulting instruction (#PF, #GP) forever.
// System calls and user faults come in later steps.
void
trap(struct trapframe *tf)
{
  switch (tf->trapno) {
  case T_IRQ0 + IRQ_TIMER:
    timerintr();
    break;
  default:
    show_regs(tf);
    for (;;) ;
  }
  // give up the CPU if this is a timer interrupt.
  // x86_64: kernel and user traps share this one entry, so
  // myproc() == 0 means the interrupt came in scheduler()'s
  // safe_halt(), where there is nothing to yield.
  if (myproc() != 0 && tf->trapno == T_IRQ0 + IRQ_TIMER)
    yield();
}
