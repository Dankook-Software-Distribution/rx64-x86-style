#include "types.h"
#include "param.h"
#include "x86.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

static void startothers(void);

// x86_64: entry.S's high64 calls this on the boot CPU only,
// at the high address with a high-address stack (riscv: start()
// jumped here in supervisor mode on all CPUs). The boot CPU sets
// everything up and then wakes the others, which enter mpenter().
void
main()
{
  acpiinit();      // CPUs from the ACPI MADT, before kinit() reuses its RAM
  seginit();       // kernel GDT at its high address
  idtinit();       // interrupt descriptor table
  printkinit();    // lock for printk
  kinit();         // physical page allocator
  kvminit();       // create kernel page table
  kvminithart();   // turn on paging
  procinit();      // process table
  trapinit();      // tickslock
  picinit();       // mask the legacy 8259 PICs
  lapicinit();     // local APIC and its timer
  ioapicinit();    // I/O APIC, every pin masked
  consoleinit();   // COM1 console; after ioapicinit for IRQ 4
  binit();         // buffer cache
  iinit();         // inode table
  fileinit();      // file table
  ideinit();       // IDE disk
  userinit();      // first user process
  startothers();   // wake the other CPUs
  scheduler();
}

// x86_64: the other CPUs start here, from entryother.S, on the
// boot page table and a stack from kalloc() (riscv: main()'s
// else branch). Memory is set up already; load this CPU's own
// registers: CR3, GDTR and TR, IDTR, and its LAPIC.
static void
mpenter(void)
{
  kvminithart();   // turn on paging
  seginit();       // this CPU's GDT and TSS
  idtinithart();   // load the shared IDT
  lapicinit();     // this CPU's local APIC and timer
  printk("cpu %d starting\n", cpuid());
  __atomic_store_n(&mycpu()->started, 1, __ATOMIC_RELEASE);
  scheduler();
}

// x86_64: copy entryother.S below 1MB and wake each other CPU
// with INIT-SIPI, one at a time (riscv: qemu started all harts).
// The stack and entry point go in the 16 bytes below the code.
static void
startothers(void)
{
  extern uchar entryother[], entryother_end[];
  uchar *code = P2V(ENTRYOTHER);

  memmove(code, entryother, entryother_end - entryother);

  for (struct cpu *c = cpus; c < cpus + ncpu; c++) {
    if (c == mycpu())
      continue;
    char *stack = kalloc();
    if (stack == 0)
      panic("startothers");
    *(uint64 *)(code - 8) = (uint64)stack + PGSIZE;
    *(uint64 *)(code - 16) = (uint64)mpenter;
    lapicstartap(c->apicid, ENTRYOTHER);
    while (__atomic_load_n(&c->started, __ATOMIC_ACQUIRE) == 0)
      ;
  }
}
