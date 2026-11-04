// x86_64: PCI configuration space and MSI-X (riscv: none; qemu
// virt put virtio at a fixed MMIO address). Each PCI function has
// 256 bytes of config space, reached through two I/O ports
// (configuration mechanism #1, Linux's pci_conf1): write the
// function and register to PCI_CONF1_ADDRESS, then read or write
// the 4 bytes at PCI_CONF1_DATA. A dev below is bus << 8 | devfn.

#include "types.h"
#include "param.h"
#include "x86.h"
#include "memlayout.h"
#include "defs.h"

#define PCI_CONF1_ADDRESS 0xCF8
#define PCI_CONF1_DATA    0xCFC

// config space registers, from Linux's uapi/linux/pci_regs.h.
#define PCI_VENDOR_ID           0x00
#define PCI_DEVICE_ID           0x02
#define PCI_COMMAND             0x04
#define  PCI_COMMAND_MEMORY     0x2   // respond to memory BARs
#define  PCI_COMMAND_MASTER     0x4   // allow the device to DMA
#define PCI_BASE_ADDRESS_0      0x10
#define  PCI_BASE_ADDRESS_SPACE_IO 0x1
#define  PCI_BASE_ADDRESS_MEM_TYPE_64 0x4
#define  PCI_BASE_ADDRESS_MEM_MASK (~0xFUL)
#define PCI_CAPABILITY_LIST     0x34

#define PCI_CAP_ID_MSIX         0x11
#define PCI_MSIX_FLAGS          2     // message control
#define  PCI_MSIX_FLAGS_ENABLE  0x8000
#define PCI_MSIX_TABLE          4     // table offset, BAR in bits 0..2
#define  PCI_MSIX_TABLE_BIR     0x7
#define PCI_MSIX_ENTRY_SIZE     16
#define PCI_MSIX_ENTRY_LOWER_ADDR 0
#define PCI_MSIX_ENTRY_UPPER_ADDR 4
#define PCI_MSIX_ENTRY_DATA     8
#define PCI_MSIX_ENTRY_VECTOR_CTRL 12 // bit 0 masks the entry

// an MSI is a memory write to the LAPIC's address range; bits
// 12..19 pick the destination APIC ID (Linux's asm/msi.h).
#define X86_MSI_BASE_ADDRESS_LOW 0xFEE00000
#define X86_MSI_DEST_SHIFT       12

static uint32
conf1_address(uint32 dev, int where)
{
  return 0x80000000 | (dev << 8) | (where & 0xFC);
}

uint32
pci_read_config_dword(uint32 dev, int where)
{
  outl(PCI_CONF1_ADDRESS, conf1_address(dev, where));
  return inl(PCI_CONF1_DATA);
}

uint16
pci_read_config_word(uint32 dev, int where)
{
  return pci_read_config_dword(dev, where) >> ((where & 2) * 8);
}

uint8
pci_read_config_byte(uint32 dev, int where)
{
  return pci_read_config_dword(dev, where) >> ((where & 3) * 8);
}

// a 2-byte write touches only those 2 bytes, so the status
// register next to the command register is not written back.
void
pci_write_config_word(uint32 dev, int where, uint16 val)
{
  outl(PCI_CONF1_ADDRESS, conf1_address(dev, where));
  outw(PCI_CONF1_DATA + (where & 2), val);
}

// find a device on bus 0 by vendor and device ID, as Linux's
// pci_get_device(). Returns its dev, or -1. qemu puts every
// device we use on bus 0, function 0.
int
pci_get_device(uint16 vendor, uint16 device)
{
  for (uint32 slot = 0; slot < 32; slot++) {
    uint32 dev = slot << 3;
    if (pci_read_config_word(dev, PCI_VENDOR_ID) == vendor &&
        pci_read_config_word(dev, PCI_DEVICE_ID) == device)
      return dev;
  }
  return -1;
}

// let the device decode its memory BARs and DMA to RAM.
void
pci_enable_device(uint32 dev)
{
  uint16 cmd = pci_read_config_word(dev, PCI_COMMAND);
  pci_write_config_word(dev, PCI_COMMAND,
                        cmd | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
}

// physical address of memory BAR bar, which the firmware chose.
uint64
pci_resource_start(uint32 dev, int bar)
{
  int where = PCI_BASE_ADDRESS_0 + 4 * bar;
  uint64 lo = pci_read_config_dword(dev, where);

  if (lo & PCI_BASE_ADDRESS_SPACE_IO)
    panic("pci_resource_start: I/O BAR");
  if (lo & PCI_BASE_ADDRESS_MEM_TYPE_64)
    lo |= (uint64)pci_read_config_dword(dev, where + 4) << 32;
  return lo & PCI_BASE_ADDRESS_MEM_MASK;
}

// offset of the first capability with this ID, or 0. The list
// starts at PCI_CAPABILITY_LIST; byte 1 of each links to the next.
int
pci_find_capability(uint32 dev, int id)
{
  int pos = pci_read_config_byte(dev, PCI_CAPABILITY_LIST) & ~3;

  while (pos != 0) {
    if (pci_read_config_byte(dev, pos) == id)
      return pos;
    pos = pci_read_config_byte(dev, pos + 1) & ~3;
  }
  return 0;
}

// send MSI-X table entry 0 to vector on the CPU with this APIC
// ID, then turn MSI-X on. The device then raises interrupts by
// writing to the LAPIC directly, with no IOAPIC pin.
void
pci_enable_msix(uint32 dev, int vector, int apicid)
{
  int pos = pci_find_capability(dev, PCI_CAP_ID_MSIX);
  if (pos == 0)
    panic("pci_enable_msix: no MSI-X");

  uint32 table = pci_read_config_dword(dev, pos + PCI_MSIX_TABLE);
  uint64 pa = pci_resource_start(dev, table & PCI_MSIX_TABLE_BIR) +
              (table & ~PCI_MSIX_TABLE_BIR);
  volatile uint32 *e = ioremap(pa, PCI_MSIX_ENTRY_SIZE);

  e[PCI_MSIX_ENTRY_LOWER_ADDR / 4] =
    X86_MSI_BASE_ADDRESS_LOW | (apicid << X86_MSI_DEST_SHIFT);
  e[PCI_MSIX_ENTRY_UPPER_ADDR / 4] = 0;
  e[PCI_MSIX_ENTRY_DATA / 4] = vector;
  e[PCI_MSIX_ENTRY_VECTOR_CTRL / 4] = 0;

  uint16 ctl = pci_read_config_word(dev, pos + PCI_MSIX_FLAGS);
  pci_write_config_word(dev, pos + PCI_MSIX_FLAGS,
                        ctl | PCI_MSIX_FLAGS_ENABLE);
}
