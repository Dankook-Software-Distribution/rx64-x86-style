//
// driver for qemu's virtio disk device.
// x86_64: uses qemu's PCI interface to virtio (riscv: mmio).
//
// qemu ... -drive file=fs.img,if=none,format=raw,id=x0 -device virtio-blk-pci,drive=x0,disable-legacy=on,addr=0x3
//

#include "types.h"
#include "x86.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "virtio.h"
#include "traps.h"
#include "proc.h"

// x86_64: register blocks found through PCI capabilities
// (riscv: R(r), a fixed offset from VIRTIO0).
static volatile struct virtio_pci_common_cfg *common;
static volatile uint16 *notify; // write the queue number to kick it

// map each virtio capability's registers (in a BAR) and keep
// the common config and queue 0's notify address.
static void
virtio_pci_find(uint32 dev)
{
  volatile uint8 *notify_base = 0;
  uint32 mult = 0;

  for (int pos = pci_find_capability(dev, PCI_CAP_ID_VNDR); pos != 0;
       pos = pci_read_config_byte(dev, pos + 1) & ~3) {
    if (pci_read_config_byte(dev, pos) != PCI_CAP_ID_VNDR)
      continue;
    int type = pci_read_config_byte(dev, pos + VIRTIO_PCI_CAP_CFG_TYPE);
    if (type != VIRTIO_PCI_CAP_COMMON_CFG &&
        type != VIRTIO_PCI_CAP_NOTIFY_CFG)
      continue;
    uint64 pa =
      pci_resource_start(dev, pci_read_config_byte(dev, pos + VIRTIO_PCI_CAP_BAR)) +
      pci_read_config_dword(dev, pos + VIRTIO_PCI_CAP_OFFSET);
    void *va = ioremap(pa, pci_read_config_dword(dev, pos + VIRTIO_PCI_CAP_LENGTH));
    if (type == VIRTIO_PCI_CAP_COMMON_CFG && common == 0)
      common = va;
    if (type == VIRTIO_PCI_CAP_NOTIFY_CFG && notify_base == 0) {
      notify_base = va;
      mult = pci_read_config_dword(dev, pos + VIRTIO_PCI_NOTIFY_MULTIPLIER);
    }
  }
  if (common == 0 || notify_base == 0)
    panic("virtio disk: no PCI capabilities");

  common->queue_select = 0;
  notify = (volatile uint16 *)(notify_base + common->queue_notify_off * mult);
}

static struct disk {
  // a set (not a ring) of DMA descriptors, with which the
  // driver tells the device where to read and write individual
  // disk operations. there are NUM descriptors.
  // most commands consist of a "chain" (a linked list) of a couple of
  // these descriptors.
  struct virtq_desc *desc;

  // a ring in which the driver writes descriptor numbers
  // that the driver would like the device to process.  it only
  // includes the head descriptor of each chain. the ring has
  // NUM elements.
  struct virtq_avail *avail;

  // a ring in which the device writes descriptor numbers that
  // the device has finished processing (just the head of each chain).
  // there are NUM used ring entries.
  struct virtq_used *used;

  // our own book-keeping.
  char free[NUM];  // is a descriptor free?
  uint16 used_idx; // we've looked this far in used[2..NUM].

  // track info about in-flight operations,
  // for use when completion interrupt arrives.
  // indexed by first descriptor index of chain.
  struct {
    struct buf *b;
    char status;
  } info[NUM];

  // disk command headers.
  // one-for-one with descriptors, for convenience.
  struct virtio_blk_req ops[NUM];

  struct spinlock vdisk_lock;

} disk;

void
virtio_disk_init(void)
{
  uint32 status = 0;
  int dev;

  initlock(&disk.vdisk_lock, "virtio_disk");

  // x86_64: find the device on the PCI bus (riscv: magic value
  // and IDs at a fixed address).
  dev = pci_get_device(PCI_VENDOR_ID_REDHAT_QUMRANET, VIRTIO_PCI_DEVICE_BLK);
  if (dev < 0)
    panic("could not find virtio disk");
  pci_enable_device(dev);
  virtio_pci_find(dev);

  // reset device
  common->device_status = status;

  // set ACKNOWLEDGE status bit
  status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
  common->device_status = status;

  // set DRIVER status bit
  status |= VIRTIO_CONFIG_S_DRIVER;
  common->device_status = status;

  // negotiate features
  // x86_64: 64 bits, read 32 at a time; a modern PCI device
  // refuses FEATURES_OK unless we accept VIRTIO_F_VERSION_1.
  common->device_feature_select = 0;
  uint64 features = common->device_feature;
  common->device_feature_select = 1;
  features |= (uint64)common->device_feature << 32;
  features &= ~(1 << VIRTIO_BLK_F_RO);
  features &= ~(1 << VIRTIO_BLK_F_SCSI);
  features &= ~(1 << VIRTIO_BLK_F_FLUSH);
  features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
  features &= ~(1 << VIRTIO_BLK_F_MQ);
  features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
  features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
  features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);
  features &= (1UL << VIRTIO_F_VERSION_1) | 0xFFFFFFFF;
  if ((features & (1UL << VIRTIO_F_VERSION_1)) == 0)
    panic("virtio disk: no VIRTIO_F_VERSION_1");
  common->guest_feature_select = 0;
  common->guest_feature = features;
  common->guest_feature_select = 1;
  common->guest_feature = features >> 32;

  // tell device that feature negotiation is complete.
  status |= VIRTIO_CONFIG_S_FEATURES_OK;
  common->device_status = status;

  // re-read status to ensure FEATURES_OK is set.
  status = common->device_status;
  if (!(status & VIRTIO_CONFIG_S_FEATURES_OK))
    panic("virtio disk FEATURES_OK unset");

  // x86_64: completions arrive as MSI-X entry 0, sent straight to
  // CPU 0's LAPIC (riscv: a PLIC IRQ). No config-change interrupts.
  pci_enable_msix(dev, T_VIRTIO, cpus[0].apicid);
  common->msix_config = VIRTIO_MSI_NO_VECTOR;

  // initialize queue 0.
  common->queue_select = 0;

  // ensure queue 0 is not in use.
  if (common->queue_enable)
    panic("virtio disk should not be ready");

  // check maximum queue size.
  uint32 max = common->queue_size;
  if (max == 0)
    panic("virtio disk has no queue 0");
  if (max < NUM)
    panic("virtio disk max queue too short");

  // allocate and zero queue memory.
  disk.desc = kalloc();
  disk.avail = kalloc();
  disk.used = kalloc();
  if (!disk.desc || !disk.avail || !disk.used)
    panic("virtio disk kalloc");
  memset(disk.desc, 0, PGSIZE);
  memset(disk.avail, 0, PGSIZE);
  memset(disk.used, 0, PGSIZE);

  // set queue size.
  common->queue_size = NUM;

  // write physical addresses.
  // x86_64: the device DMAs, so V2P (riscv: identity map).
  common->queue_desc_lo = V2P(disk.desc);
  common->queue_desc_hi = V2P(disk.desc) >> 32;
  common->queue_avail_lo = V2P(disk.avail);
  common->queue_avail_hi = V2P(disk.avail) >> 32;
  common->queue_used_lo = V2P(disk.used);
  common->queue_used_hi = V2P(disk.used) >> 32;

  common->queue_msix_vector = 0;
  if (common->queue_msix_vector != 0)
    panic("virtio disk: MSI-X vector");

  // queue is ready.
  common->queue_enable = 1;

  // all NUM descriptors start out unused.
  for (int i = 0; i < NUM; i++)
    disk.free[i] = 1;

  // tell device we're completely ready.
  status |= VIRTIO_CONFIG_S_DRIVER_OK;
  common->device_status = status;
}

// find a free descriptor, mark it non-free, return its index.
static int
alloc_desc()
{
  for (int i = 0; i < NUM; i++) {
    if (disk.free[i]) {
      disk.free[i] = 0;
      return i;
    }
  }
  return -1;
}

// mark a descriptor as free.
static void
free_desc(int i)
{
  if (i >= NUM)
    panic("free_desc 1");
  if (disk.free[i])
    panic("free_desc 2");
  disk.desc[i].addr = 0;
  disk.desc[i].len = 0;
  disk.desc[i].flags = 0;
  disk.desc[i].next = 0;
  disk.free[i] = 1;
  wakeup(&disk.free[0]);
}

// free a chain of descriptors.
static void
free_chain(int i)
{
  while (1) {
    int flag = disk.desc[i].flags;
    int nxt = disk.desc[i].next;
    free_desc(i);
    if (flag & VRING_DESC_F_NEXT)
      i = nxt;
    else
      break;
  }
}

// allocate three descriptors (they need not be contiguous).
// disk transfers always use three descriptors.
static int
alloc3_desc(int *idx)
{
  for (int i = 0; i < 3; i++) {
    idx[i] = alloc_desc();
    if (idx[i] < 0) {
      for (int j = 0; j < i; j++)
        free_desc(idx[j]);
      return -1;
    }
  }
  return 0;
}

void
virtio_disk_rw(struct buf *b, int write)
{
  uint64 sector = b->blockno * (BSIZE / 512);

  acquire(&disk.vdisk_lock);

  // the spec's Section 5.2 says that legacy block operations use
  // three descriptors: one for type/reserved/sector, one for the
  // data, one for a 1-byte status result.

  // allocate the three descriptors.
  int idx[3];
  while (1) {
    if (alloc3_desc(idx) == 0) {
      break;
    }
    sleep_prepare(&disk.free[0]);
    release(&disk.vdisk_lock);
    sleep();
    acquire(&disk.vdisk_lock);
  }

  // format the three descriptors.
  // qemu's virtio-blk.c reads them.

  struct virtio_blk_req *buf0 = &disk.ops[idx[0]];

  if (write)
    buf0->type = VIRTIO_BLK_T_OUT; // write the disk
  else
    buf0->type = VIRTIO_BLK_T_IN; // read the disk
  buf0->reserved = 0;
  buf0->sector = sector;

  disk.desc[idx[0]].addr = V2P(buf0); // x86_64: V2P for DMA
  disk.desc[idx[0]].len = sizeof(struct virtio_blk_req);
  disk.desc[idx[0]].flags = VRING_DESC_F_NEXT;
  disk.desc[idx[0]].next = idx[1];

  disk.desc[idx[1]].addr = V2P(b->data);
  disk.desc[idx[1]].len = BSIZE;
  if (write)
    disk.desc[idx[1]].flags = 0; // device reads b->data
  else
    disk.desc[idx[1]].flags = VRING_DESC_F_WRITE; // device writes b->data
  disk.desc[idx[1]].flags |= VRING_DESC_F_NEXT;
  disk.desc[idx[1]].next = idx[2];

  disk.info[idx[0]].status = 0xff; // device writes 0 on success
  disk.desc[idx[2]].addr = V2P(&disk.info[idx[0]].status);
  disk.desc[idx[2]].len = 1;
  disk.desc[idx[2]].flags = VRING_DESC_F_WRITE; // device writes the status
  disk.desc[idx[2]].next = 0;

  // record struct buf for virtio_disk_intr().
  b->disk = 1;
  disk.info[idx[0]].b = b;

  // tell the device the first index in our chain of descriptors.
  disk.avail->ring[disk.avail->idx % NUM] = idx[0];

  __sync_synchronize(); // x86_64: riscv io_fence()

  // tell the device another avail ring entry is available.
  disk.avail->idx += 1; // not % NUM ...

  __sync_synchronize(); // x86_64: riscv io_fence()

  *notify = 0; // value is queue number

  // Wait for virtio_disk_intr() to say request has finished.
  while (b->disk == 1) {
    sleep_prepare(b);
    release(&disk.vdisk_lock);
    sleep();
    acquire(&disk.vdisk_lock);
  }

  disk.info[idx[0]].b = 0;
  free_chain(idx[0]);

  release(&disk.vdisk_lock);
}

void
virtio_disk_intr()
{
  acquire(&disk.vdisk_lock);

  // x86_64: an MSI-X message needs no ack (riscv: write
  // INTERRUPT_ACK). New completions may race with this loop;
  // the next interrupt then finds nothing, which is harmless.

  // the device increments disk.used->idx when it
  // adds an entry to the used ring.

  while (disk.used_idx != disk.used->idx) {
    __sync_synchronize(); // x86_64: riscv io_fence()
    int id = disk.used->ring[disk.used_idx % NUM].id;

    if (disk.info[id].status != 0)
      panic("virtio_disk_intr status");

    struct buf *b = disk.info[id].b;
    b->disk = 0; // disk is done with buf
    wakeup(b);

    disk.used_idx += 1;
  }

  release(&disk.vdisk_lock);
}
