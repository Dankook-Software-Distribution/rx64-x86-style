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

extern char trapret[];
extern void forkret(void);
static void freeproc(struct proc *p);

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
// x86_64: at KSTACKBASE above the RAM map (riscv: below
// TRAMPOLINE). kalloc() returns a virtual address, hence V2P().
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
// x86_64: riscv read the hart id from tp. Only the boot CPU runs
// for now; step 8 looks the CPU up by its LAPIC ID.
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
static struct proc *
allocproc(void)
{
  struct proc *p = find_unused();
  if (p == 0)
    return 0;

  p->pid = allocpid();
  p->state = USED;

  // x86_64: the trapframe is the top of the kernel stack, where
  // the CPU pushes it from TSS sp0 (riscv: a page of its own).
  p->trapframe = (struct trapframe *)(p->kstack + PGSIZE) - 1;
  // x86_64: trapret pops all of it into user registers.
  memset(p->trapframe, 0, sizeof(*p->trapframe));

  // A user page table that maps only the kernel.
  p->pagetable = proc_pagetable(p);
  if (p->pagetable == 0) {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret.
  // x86_64: swtch() jumps to forkret, so the 8-byte slot below the
  // trapframe stands in for a return address: forkret returns to
  // trapret, which pops the trapframe and does iretq.
  memset(&p->context, 0, sizeof(p->context));
  p->context.rsp = (uint64)(p->trapframe) - 8;
  *(uint64 *)p->context.rsp = (uint64)trapret;
  p->context.rip = (uint64)forkret;
  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
// x86_64: no kfree(p->trapframe); it is part of the kernel stack.
static void
freeproc(struct proc *p)
{
  if (p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory.
// x86_64: no trampoline or trapframe pages to map (riscv); the
// kernel half comes from uvmcreate().
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  pagetable = uvmcreate();
  if (pagetable == 0)
    return 0;

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
// x86_64: no trampoline or trapframe mappings to remove first.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmfree(pagetable, sz);
}

// TEST(7-a): user processes made by hand, without exec. Each
// runs the machine code below at user address 0, with one page
// of code (read-only, executable) and one page of stack above
// it (PTE_RW | PTE_XD). Expect "syscall 7" now and then, and for
// syscall 1..5 one fault each from cs 0x23:
//   1 trap e err 7  cr2 0         write to the read-only code page
//   2 trap e err 4  cr2 100000    read an unmapped page
//   3 trap d err 0                cli with IOPL 0
//   4 trap d err 72 (qemu: e2)    int $14: vector 14 has DPL 0
//   5 trap e err 15 cr2 1000      execute the XD stack page
static uchar test_loop[] = {
  0xb8, 0x07, 0x00, 0x00, 0x00, // 0:  mov $7, %eax
  0xb9, 0x00, 0x00, 0x00, 0x01, //     mov $0x1000000, %ecx
  0xff, 0xc9,                   // 1:  dec %ecx
  0x75, 0xfc,                   //     jnz 1b
  0xcd, 0x40,                   //     int $64
  0xeb, 0xee,                   //     jmp 0b
};
static uchar test_wr[] = {
  0xb8, 0x01, 0x00, 0x00, 0x00, 0xcd, 0x40, // mov $1, %eax; int $64
  0xc6, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00, // movb $0, 0
};
static uchar test_rd[] = {
  0xb8, 0x02, 0x00, 0x00, 0x00, 0xcd, 0x40, // mov $2, %eax; int $64
  0x8b, 0x04, 0x25, 0x00, 0x00, 0x10, 0x00, // mov 0x100000, %eax
};
static uchar test_cli[] = {
  0xb8, 0x03, 0x00, 0x00, 0x00, 0xcd, 0x40, // mov $3, %eax; int $64
  0xfa,                                     // cli
};
static uchar test_int14[] = {
  0xb8, 0x04, 0x00, 0x00, 0x00, 0xcd, 0x40, // mov $4, %eax; int $64
  0xcd, 0x0e,                               // int $14
};
static uchar test_xd[] = {
  0xb8, 0x05, 0x00, 0x00, 0x00, 0xcd, 0x40, // mov $5, %eax; int $64
  0xb8, 0x00, 0x10, 0x00, 0x00,             // mov $0x1000, %eax
  0xff, 0xe0,                               // jmp *%rax
};

static void
test_user(uchar *code, int n)
{
  struct proc *p;

  if ((p = allocproc()) == 0)
    panic("user_test");
  if (uvmalloc(p->pagetable, 0, PGSIZE, 0) == 0 ||
      uvmalloc(p->pagetable, PGSIZE, 2 * PGSIZE, PTE_RW | PTE_XD) == 0)
    panic("user_test: uvmalloc");
  // copyout() refuses a read-only page, so write it directly.
  memmove(P2V(walkaddr(p->pagetable, 0)), code, n);
  p->sz = 2 * PGSIZE;

  p->trapframe->rip = 0;
  p->trapframe->rsp = 2 * PGSIZE;
  p->trapframe->cs = UCSEG | 3;
  p->trapframe->ss = UDSEG | 3;
  p->trapframe->rflags = FL_IF;
  safestrcpy(p->name, "user_test", sizeof(p->name));

  p->state = RUNNABLE;
  release(&p->lock);
}

void
user_test(void)
{
  test_user(test_loop, sizeof(test_loop));
  test_user(test_wr, sizeof(test_wr));
  test_user(test_rd, sizeof(test_rd));
  test_user(test_cli, sizeof(test_cli));
  test_user(test_int14, sizeof(test_int14));
  test_user(test_xd, sizeof(test_xd));
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
    // x86_64: no intr_on() first (riscv): sti; cli takes none.
    // Interrupts come in safe_halt() and in processes.
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
        // x86_64: a trap from user mode pushes the trapframe at
        // TSS sp0 (riscv: trampoline.S read the stack pointer).
        c->tss.sp0 = p->kstack + PGSIZE;
        // x86_64: switch page tables here (riscv: userret did).
        write_cr3(V2P(p->pagetable));
        swtch(&c->context, &p->context);
        // x86_64: back to the kernel's, so wait() can free p's.
        kvminithart();

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
      // x86_64: hlt with IF=0 never wakes (riscv's wfi does), so
      // safe_halt() does sti; hlt.
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
// x86_64: returns into trapret through the slot allocproc() set
// up (riscv: jumps to userret in the trampoline).
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
