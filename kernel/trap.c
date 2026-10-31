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
// The timer is periodic, but each interrupt needs an EOI or no
// more come (riscv: writing stimecmp cleared and rearmed it).
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

// x86_64: print an unexpected trap (riscv: kerneltrap() and
// usertrap() printed scause, sepc and stval). Uses earlyputs, not
// printk, so a fault while printk holds its lock cannot deadlock.
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

// x86_64: from user space? The low 2 bits of cs are the CPL
// (riscv: SPP in sstatus).
static int
user_mode(struct trapframe *tf)
{
  return (tf->cs & 3) == 3;
}

// x86_64: every trap comes here from alltraps, from the kernel or
// from user space (riscv: usertrap() and kerneltrap()), and
// returns through trapret. tf->trapno replaces scause.
// An unknown trap from the kernel is a bug: print it and stop.
void
trap(struct trapframe *tf)
{
  switch (tf->trapno) {
  case T_IRQ0 + IRQ_TIMER:
    timerintr();
    break;
  case T_SYSCALL:
    // the interrupt gate turned interrupts off.
    intr_on();
    // TEST(7-a): syscall() comes in 7-b. The CPU must have built
    // the frame at TSS sp0, which is where allocproc() put it.
    printk("syscall %ld\n", tf->rax);
    if (tf != myproc()->trapframe)
      panic("trap: tf != p->trapframe");
    break;
  default:
    show_regs(tf);
    if (user_mode(tf)) {
      // TEST(7-a): stop only this process until kexit() in 7-c.
      // Not UNUSED: we are still on its kernel stack.
      struct proc *p = myproc();
      acquire(&p->lock);
      p->state = ZOMBIE;
      sched();
    }
    for (;;) ;
  }
  // give up the CPU if this is a timer interrupt.
  // x86_64: myproc() is 0 if it came in scheduler()'s safe_halt().
  if (myproc() != 0 && tf->trapno == T_IRQ0 + IRQ_TIMER)
    yield();
}
