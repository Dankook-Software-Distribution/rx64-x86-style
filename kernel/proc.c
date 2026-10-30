#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "x86.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
// x86_64: the stacks sit just above the RAM mapping at KSTACKBASE
// (riscv: beneath TRAMPOLINE), so an overflow faults on the guard
// page instead of silently overwriting the direct map, like
// Linux's VMAP_STACK. kalloc() returns a kernel virtual address
// (riscv: the physical address, hence its name pa), so the
// variable is named stack and the PTE gets V2P(stack). Stacks
// are writable and never executed: PTE_RW | PTE_XD (riscv:
// PTE_R | PTE_W; x86 has no R bit, and XD forbids execution
// where riscv's X would allow it).
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    char *stack = kalloc();
    if (stack == 0)
      panic("kalloc");
    uint64 va = KSTACK((int)(p - proc));
    kvmmap(kpgtbl, va, V2P(stack), PGSIZE, PTE_RW | PTE_XD);
  }
}

void
procinit(void)
{
  struct proc *p;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");

  for (p = proc; p < &proc[NPROC]; p++) {
    initlock(&p->lock, "proc");
    p->state = UNUSED;
    p->kstack = KSTACK((int)(p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
// x86_64: riscv read the hart id from tp. For now only the boot
// CPU runs (-smp 1) and it is CPU 0, as Linux treats the boot CPU
// before per-CPU data is set up. Replace this when other CPUs
// start: x86 can find the current CPU by its LAPIC ID (through a
// table from APIC ID to cpus[] index) or by gs, and the choice
// waits until then.
int
cpuid()
{
  return 0;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu *
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

struct proc *
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;

  acquire(&pid_lock);
  pid = nextpid;
  nextpid += 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, return it with p->lock held; otherwise return 0.
// x86_64: split out of allocproc() (riscv: a loop with goto found).
static struct proc *
find_unused(void)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state == UNUSED)
      return p;
    release(&p->lock);
  }

  return 0;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, return 0.
// x86_64: no user page table yet; proc_pagetable() comes back
// in step 6.
static struct proc *
allocproc(void)
{
  struct proc *p = find_unused();
  if (p == 0)
    return 0;

  p->pid = allocpid();
  p->state = USED;

  // x86_64: the trapframe is the top of the kernel stack
  // (riscv: a page of its own from kalloc(), so that
  // trampoline.S could reach it). On a trap from user mode the
  // CPU switches to the TSS rsp0, which the scheduler sets to
  // p->kstack + PGSIZE, and pushes the frame from there down.
  // It belongs to the stack page, so freeproc() must not
  // kfree() it.
  p->trapframe = (struct trapframe *)(p->kstack + PGSIZE) - 1;

  // Set up new context to start executing at forkret.
  // x86_64: swtch() enters forkret with jmp, which pushes no
  // return address, so leave an 8-byte slot below the trapframe
  // in its place. That gives rsp % 16 == 8 at function entry, as
  // the ABI expects (the trapframe is 16-byte aligned since
  // sizeof(struct trapframe) is 176). Step 7 puts trapret's
  // address in the slot, so forkret returns into trapret.
  memset(&p->context, 0, sizeof(p->context));
  p->context.rsp = (uint64)(p->trapframe) - 8;
  p->context.rip = (uint64)forkret;
  return p;
}

// TEST(5): two kernel threads that the timer preempts.
// forkret() returns into the function stored in the slot below
// the trapframe. scheduler() hands them interrupts off, so they
// turn them on. They must never return. Expect a and b mixed
// on the serial port.
static void
test_thread(char c)
{
  intr_on();
  for (;;) {
    for (volatile int i = 0; i < 10000000; i++)
      ;
    printk("%c", c);
  }
}

static void
test_a(void)
{
  test_thread('a');
}

static void
test_b(void)
{
  test_thread('b');
}

void
kthread_test(void)
{
  void (*fn[])(void) = { test_a, test_b };
  struct proc *p;

  for (int i = 0; i < 2; i++) {
    if ((p = allocproc()) == 0)
      panic("kthread_test");
    *(uint64 *)p->context.rsp = (uint64)fn[i];
    p->state = RUNNABLE;
    release(&p->lock);
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for (;;) {
    // Turn interrupts off to avoid a possible race between an
    // interrupt and hlt.
    // x86_64: riscv first does intr_on() so that pending
    // interrupts get taken, but sti holds them off until after
    // the next instruction, so sti; cli would take none. As in
    // Linux's do_idle(), interrupts are taken by safe_halt()
    // below and by processes, which run with interrupts on.
    intr_off();

    int found = 0;
    for (p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if (p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        // x86_64: on a trap from CPL 3 the CPU loads rsp from
        // the TSS's sp0 before pushing the trapframe, so point
        // it at the top of p's kernel stack, where allocproc()
        // placed p->trapframe. riscv has no such register:
        // trampoline.S read the stack from the trapframe page.
        // Linux does the same in __switch_to()
        // (update_task_stack()).
        c->tss.sp0 = p->kstack + PGSIZE;
        swtch(&c->context, &p->context);

        // Don't re-enable interrupts on release.
        mycpu()->intena = 0;

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
        found = 1;
      }
      release(&p->lock);
    }
    if (found == 0) {
      // nothing to run; stop running on this core until an interrupt.
      // x86_64: riscv's wfi wakes on a pending interrupt even
      // with interrupts off, but hlt with IF=0 never wakes, so
      // safe_halt() enables interrupts and halts in one step.
      safe_halt();
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&p->lock))
    panic("sched p->lock");
  if (mycpu()->noff != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched RUNNING");
  if (intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
// x86_64: riscv jumps to userret in the trampoline here. Ours
// just returns, into the slot allocproc() left below the
// trapframe (trapret in step 7).
void
forkret(void)
{
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);
}

// Register current process as waiting for wakeups on chan.
void
sleep_prepare(void *chan)
{
  struct proc *p = myproc();

  acquire(&p->lock);
  if (chan == 0)
    panic("sleep_prepare: zero chan");
  p->chan = chan;
  release(&p->lock);
}

// Put the thread to sleep.  Assumes sleep_prepare() was called before.
// If the channel registered by sleep_prepare() has been woken up in
// the meantime, do not go to sleep, and instead return immediately.
void
sleep(void)
{
  struct proc *p = myproc();

  acquire(&p->lock);
  if (p->chan != 0) {
    p->state = SLEEPING;
    sched();
  }
  release(&p->lock);
}

// Wake up all processes sleeping on channel chan.
void
wakeup(void *chan)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->chan == chan) {
      // If the process is waiting for wakeups on this channel,
      // signal that the wakeup happened by clearing p->chan.
      p->chan = 0;

      // If this waiting process has gotten so far as to actually
      // go to sleep, also set it back to RUNNING.
      if (p->state == SLEEPING) {
        p->state = RUNNABLE;
      }
    }
    release(&p->lock);
  }
}
