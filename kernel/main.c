#include "types.h"
#include "param.h"
#include "x86.h"
#include "memlayout.h"
#include "defs.h"

volatile static int started = 0;

// x86_64: entry.S's high64 calls this on the boot CPU only,
// at the high address with a high-address stack (riscv: start()
// jumped here in supervisor mode on all CPUs).
void
main()
{
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
  kthread_test();  // TEST(5): two kernel threads
  scheduler();
}
