// Saved registers for kernel context switches.
// x86_64: rip and rsp (riscv: ra and sp); swtch pops the return
// address into rip. swtch.S uses these offsets.
struct context {
  /*  0 */ uint64 rip;
  /*  8 */ uint64 rsp;

  // callee-saved
  /* 16 */ uint64 rbx;
  /* 24 */ uint64 rbp;
  /* 32 */ uint64 r12;
  /* 40 */ uint64 r13;
  /* 48 */ uint64 r14;
  /* 56 */ uint64 r15;
};
_Static_assert(sizeof(struct context) == 64,
               "context layout must match swtch.S");

// Per-CPU state.
struct cpu {
  struct proc *proc;      // The process running on this cpu, or null.
  struct context context; // swtch() here to enter scheduler().
  int noff;               // Depth of push_off() nesting.
  int intena;             // Were interrupts enabled before push_off()?

  // x86_64: each CPU needs its own TSS, and so its own GDT for
  // the busy TSS descriptor (riscv: no segmentation).
  uint64 gdt[NSEGS];      // This CPU's GDT, from seginit().
  struct x86_hw_tss tss;  // This CPU's TSS; sp0 is set on each switch.
  uint8 apicid;           // x86_64: LAPIC ID, from acpiinit().
  int started;            // x86_64: set by an AP once it runs.
};

extern struct cpu cpus[NCPU];
extern int ncpu;

// registers saved on a trap, at the top of the process's kernel
// stack. built from the top down: the CPU pushes ss..rip (and err
// for some vectors), the stub in vectors.S pushes trapno (and a
// zero err otherwise), and alltraps in trapasm.S pushes r15..rax.
// trapret pops it in reverse and finishes with iretq.
//
// x86_64: riscv kept it in its own page under the trampoline,
// with kernel_satp and friends; here the CPU finds the stack in
// TSS sp0. trapasm.S uses these offsets as numbers.
struct trapframe {
  // pushed by alltraps
  /*   0 */ uint64 rax;
  /*   8 */ uint64 rbx;
  /*  16 */ uint64 rcx;
  /*  24 */ uint64 rdx;
  /*  32 */ uint64 rbp;
  /*  40 */ uint64 rsi;
  /*  48 */ uint64 rdi;
  /*  56 */ uint64 r8;
  /*  64 */ uint64 r9;
  /*  72 */ uint64 r10;
  /*  80 */ uint64 r11;
  /*  88 */ uint64 r12;
  /*  96 */ uint64 r13;
  /* 104 */ uint64 r14;
  /* 112 */ uint64 r15;

  // pushed by the vector stub in vectors.S
  /* 120 */ uint64 trapno;
  /* 128 */ uint64 err;         // pushed by the CPU for some vectors

  // pushed by the CPU
  /* 136 */ uint64 rip;
  /* 144 */ uint16 cs;          // only low 16 bits are meaningful
            uint16 padding[3];
  /* 152 */ uint64 rflags;
  /* 160 */ uint64 rsp;
  /* 168 */ uint64 ss;
};
_Static_assert(sizeof(struct trapframe) == 176,
               "trapframe layout must match trapasm.S");

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Per-process state
struct proc {
  struct spinlock lock;

  // p->lock must be held when using these:
  enum procstate state; // Process state
  void *chan;           // If non-zero, sleeping on chan
  int killed;           // If non-zero, have been killed
  int xstate;           // Exit status to be returned to parent's wait
  int pid;              // Process ID

  // wait_lock must be held when using this:
  struct proc *parent; // Parent process

  // these are private to the process, so p->lock need not be held.
  uint64 kstack;               // Virtual address of kernel stack
  uint64 sz;                   // Size of process memory (bytes)
  pagetable_t pagetable;       // User page table
  struct trapframe *trapframe; // top of kstack; x86_64: was a page for trampoline.S
  struct context context;      // swtch() here to run process
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)
};
