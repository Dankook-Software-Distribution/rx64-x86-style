// x86_64: find the CPUs in the ACPI MADT, as Linux does (riscv:
// qemu started CPUS harts and each read its hart id from tp).
// The firmware leaves a Root System Description Pointer in low
// memory; it points to the RSDT, a list of tables, one of which
// ("APIC", the MADT) has an entry for each CPU's local APIC.
// Table layouts follow the ACPI spec and Linux's ACPICA names
// (include/acpi/actbl*.h); only the fields used here matter.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "x86.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct acpi_table_rsdp {
  char signature[8]; // "RSD PTR "
  uint8 checksum;    // first 20 bytes sum to 0
  char oem_id[6];
  uint8 revision;
  uint32 rsdt_physical_address;
} __attribute__((packed));

struct acpi_table_header { // starts every table
  char signature[4];
  uint32 length; // of the whole table, header included
  uint8 revision;
  uint8 checksum;
  char oem_id[6];
  char oem_table_id[8];
  uint32 oem_revision;
  char asl_compiler_id[4];
  uint32 asl_compiler_revision;
} __attribute__((packed));

struct acpi_table_madt {
  struct acpi_table_header header; // signature "APIC"
  uint32 address;                  // local APIC physical address
  uint32 flags;
} __attribute__((packed)); // entries follow

struct acpi_subtable_header {
  uint8 type;
  uint8 length;
} __attribute__((packed));

#define ACPI_MADT_TYPE_LOCAL_APIC 0

struct acpi_madt_local_apic {
  struct acpi_subtable_header header;
  uint8 processor_id;
  uint8 id; // local APIC ID
  uint32 lapic_flags;
} __attribute__((packed));

#define ACPI_MADT_ENABLED 1 // lapic_flags: the CPU is usable

int ncpu; // number of CPUs in cpus[]

static uint8
sum(void *addr, int len)
{
  uint8 *p = addr, s = 0;

  for (int i = 0; i < len; i++)
    s += p[i];
  return s;
}

static struct acpi_table_rsdp *
rsdp_search(uint64 pa, int len)
{
  for (uint8 *p = P2V(pa); p < (uint8 *)P2V(pa + len); p += 16)
    if (memcmp(p, "RSD PTR ", 8) == 0 && sum(p, 20) == 0)
      return (struct acpi_table_rsdp *)p;
  return 0;
}

// look where Linux's acpi_find_root_pointer() does: the first 1KB
// of the EBDA (its segment is at 0x40E), then the BIOS ROM area.
static struct acpi_table_rsdp *
rsdp_find(void)
{
  struct acpi_table_rsdp *rsdp;
  uint64 ebda = (uint64)*(uint16 *)P2V(0x40E) << 4;

  if (ebda && (rsdp = rsdp_search(ebda, 1024)) != 0)
    return rsdp;
  return rsdp_search(0xE0000, 0x20000);
}

static struct acpi_table_madt *
madt_find(void)
{
  struct acpi_table_rsdp *rsdp;
  struct acpi_table_header *rsdt, *h;
  uint32 *entry;
  int n;

  if ((rsdp = rsdp_find()) == 0)
    panic("acpi: no RSDP");
  rsdt = P2V((uint64)rsdp->rsdt_physical_address);
  if (memcmp(rsdt->signature, "RSDT", 4) != 0 ||
      sum(rsdt, rsdt->length) != 0)
    panic("acpi: bad RSDT");

  // the RSDT's body is an array of 32-bit table addresses.
  entry = (uint32 *)(rsdt + 1);
  n = (rsdt->length - sizeof(*rsdt)) / sizeof(uint32);
  for (int i = 0; i < n; i++) {
    h = P2V((uint64)entry[i]);
    if (memcmp(h->signature, "APIC", 4) == 0)
      return (struct acpi_table_madt *)h;
  }
  panic("acpi: no MADT");
}

// fill cpus[].apicid and ncpu from the MADT. Runs first in main(),
// on the boot page table (which maps all of low RAM) and before
// kinit() can hand out the pages the tables are in.
void
acpiinit(void)
{
  struct acpi_table_madt *madt = madt_find();
  uint8 *p = (uint8 *)(madt + 1);
  uint8 *end = (uint8 *)madt + madt->header.length;

  for (; p < end; p += ((struct acpi_subtable_header *)p)->length) {
    struct acpi_madt_local_apic *lapic = (struct acpi_madt_local_apic *)p;
    if (lapic->header.type != ACPI_MADT_TYPE_LOCAL_APIC ||
        (lapic->lapic_flags & ACPI_MADT_ENABLED) == 0)
      continue;
    if (ncpu < NCPU)
      cpus[ncpu++].apicid = lapic->id;
  }
  if (ncpu == 0)
    panic("acpi: no CPUs");
}
