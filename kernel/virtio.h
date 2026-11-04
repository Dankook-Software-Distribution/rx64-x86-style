//
// virtio device definitions.
// for both the PCI interface, and virtio descriptors.
// only tested with qemu.
//
// the virtio spec:
// https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.pdf
//

// x86_64: virtio over PCI (riscv: virtio-mmio at 0x10001000).
// The registers are in a memory BAR; vendor-specific PCI
// capabilities say which BAR and where. From Linux's
// uapi/linux/virtio_pci.h.
#define PCI_VENDOR_ID_REDHAT_QUMRANET 0x1AF4
#define VIRTIO_PCI_DEVICE_BLK         0x1042 // 0x1040 + device type 2
#define PCI_CAP_ID_VNDR               0x09

// struct virtio_pci_cap: cfg_type at 3, bar at 4, offset at 8,
// length at 12; the notify cap adds a multiplier at 16.
#define VIRTIO_PCI_CAP_CFG_TYPE       3
#define VIRTIO_PCI_CAP_BAR            4
#define VIRTIO_PCI_CAP_OFFSET         8
#define VIRTIO_PCI_CAP_LENGTH         12
#define VIRTIO_PCI_NOTIFY_MULTIPLIER  16
#define VIRTIO_PCI_CAP_COMMON_CFG     1
#define VIRTIO_PCI_CAP_NOTIFY_CFG     2

#define VIRTIO_MSI_NO_VECTOR          0xFFFF

// the common configuration registers, in place of the mmio ones.
struct virtio_pci_common_cfg {
  uint32 device_feature_select; // which 32 feature bits to read
  uint32 device_feature;
  uint32 guest_feature_select;  // which 32 feature bits to write
  uint32 guest_feature;
  uint16 msix_config;           // MSI-X entry for config changes
  uint16 num_queues;
  uint8 device_status;
  uint8 config_generation;
  uint16 queue_select;          // the queue the rest refer to
  uint16 queue_size;            // reads the maximum at first
  uint16 queue_msix_vector;     // MSI-X entry for this queue
  uint16 queue_enable;
  uint16 queue_notify_off;
  uint32 queue_desc_lo;         // physical addresses of the rings
  uint32 queue_desc_hi;
  uint32 queue_avail_lo;
  uint32 queue_avail_hi;
  uint32 queue_used_lo;
  uint32 queue_used_hi;
};
_Static_assert(sizeof(struct virtio_pci_common_cfg) == 56, "common_cfg");

// status register bits, from qemu virtio_config.h
#define VIRTIO_CONFIG_S_ACKNOWLEDGE 1
#define VIRTIO_CONFIG_S_DRIVER      2
#define VIRTIO_CONFIG_S_DRIVER_OK   4
#define VIRTIO_CONFIG_S_FEATURES_OK 8

// device feature bits
#define VIRTIO_BLK_F_RO             5  /* Disk is read-only */
#define VIRTIO_BLK_F_SCSI           7  /* Supports scsi command passthru */
#define VIRTIO_BLK_F_FLUSH          9  /* Cache flush command supported */
#define VIRTIO_BLK_F_CONFIG_WCE     11 /* Writeback mode available in config */
#define VIRTIO_BLK_F_MQ             12 /* support more than one vq */
#define VIRTIO_F_ANY_LAYOUT         27
#define VIRTIO_RING_F_INDIRECT_DESC 28
#define VIRTIO_RING_F_EVENT_IDX     29
#define VIRTIO_F_VERSION_1          32 // x86_64: required by modern PCI

// this many virtio descriptors.
// must be a power of two.
#define NUM 8

// a single descriptor, from the spec.
struct virtq_desc {
  uint64 addr;
  uint32 len;
  uint16 flags;
  uint16 next;
};
#define VRING_DESC_F_NEXT  1 // chained with another descriptor
#define VRING_DESC_F_WRITE 2 // device writes (vs read)

// the (entire) avail ring, from the spec.
struct virtq_avail {
  uint16 flags;     // always zero
  uint16 idx;       // driver will write ring[idx] next
  uint16 ring[NUM]; // descriptor numbers of chain heads
  uint16 unused;
};

// one entry in the "used" ring, with which the
// device tells the driver about completed requests.
struct virtq_used_elem {
  uint32 id; // index of start of completed descriptor chain
  uint32 len;
};

struct virtq_used {
  uint16 flags; // always zero
  uint16 idx;   // device increments when it adds a ring[] entry
  struct virtq_used_elem ring[NUM];
};

// these are specific to virtio block devices, e.g. disks,
// described in Section 5.2 of the spec.

#define VIRTIO_BLK_T_IN  0 // read the disk
#define VIRTIO_BLK_T_OUT 1 // write the disk

// the format of the first descriptor in a disk request.
// to be followed by two more descriptors containing
// the block, and a one-byte status.
struct virtio_blk_req {
  uint32 type; // VIRTIO_BLK_T_IN or ..._OUT
  uint32 reserved;
  uint64 sector;
};
