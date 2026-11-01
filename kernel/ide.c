#include "types.h"
#include "param.h"
#include "x86.h"
#include "memlayout.h"
#include "traps.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "defs.h"

// x86_64: IDE port offsets from IDE_BASE (riscv: virtio MMIO
// registers). Names follow Linux include/linux/ata.h.
#define ATA_REG_DATA   0 // data, 4 bytes at a time with insl/outsl
#define ATA_REG_NSECT  2 // sector count
#define ATA_REG_LBAL   3 // LBA bits 0..7
#define ATA_REG_LBAM   4 // LBA bits 8..15
#define ATA_REG_LBAH   5 // LBA bits 16..23
#define ATA_REG_DEVICE 6 // drive select, LBA mode, LBA bits 24..27
#define ATA_REG_STATUS 7 // status when read
#define ATA_REG_CMD    7 // command when written

#define ATA_BUSY 0x80
#define ATA_DRDY 0x40
#define ATA_DF   0x20
#define ATA_DRQ  0x08
#define ATA_ERR  0x01

#define ATA_CMD_READ_MULTI  0xC4
#define ATA_CMD_WRITE_MULTI 0xC5

static struct {
  struct spinlock lock;
  struct buf *cur;
  int write;
} ide;

// wait until the disk is not busy and bit (ATA_DRDY or ATA_DRQ)
// is set. returns the last status read.
static uint8
idewait(uint8 bit)
{
  uint8 s;

  while (((s = inb(IDE_BASE + ATA_REG_STATUS)) & (ATA_BUSY | bit)) != bit)
    ;
  return s;
}

static void
idestart(struct buf *b, int write)
{
  uint sector = b->blockno * (BSIZE / 512);
  idewait(ATA_DRDY);
  outb(IDE_CTL, 0);
  outb(IDE_BASE + ATA_REG_NSECT, BSIZE / 512);
  outb(IDE_BASE + ATA_REG_LBAL, sector);
  outb(IDE_BASE + ATA_REG_LBAM, sector >> 8);
  outb(IDE_BASE + ATA_REG_LBAH, sector >> 16);
  outb(IDE_BASE + ATA_REG_DEVICE, 0xE0 | ((sector >> 24) & 0x0F));

  if (write) {
    outb(IDE_BASE + ATA_REG_CMD, ATA_CMD_WRITE_MULTI);
    idewait(ATA_DRQ);
    outsl(IDE_BASE + ATA_REG_DATA, b->data, BSIZE / 4);
  }
  else {
    outb(IDE_BASE + ATA_REG_CMD, ATA_CMD_READ_MULTI);
  }
}

void
iderw(struct buf *b, int write)
{
  acquire(&ide.lock);
  while (ide.cur != 0) {
    sleep_prepare(&ide);
    release(&ide.lock);
    sleep();
    acquire(&ide.lock);
  }
  ide.cur = b;
  ide.write = write;
  b->disk = 1;
  idestart(b, write);
  while (b->disk == 1) {
    sleep_prepare(b);
    release(&ide.lock);
    sleep();
    acquire(&ide.lock);
  }
  ide.cur = 0;
  wakeup(&ide);
  release(&ide.lock);
}

void
ideintr(void)
{
  acquire(&ide.lock);
  uint8 s = inb(IDE_BASE + ATA_REG_STATUS);
  struct buf *b = ide.cur;
  if (b == 0) {
    release(&ide.lock);
    return;
  }
  if (s & (ATA_DF | ATA_ERR))
    panic("ideintr: disk error");
  if (!ide.write)
    insl(IDE_BASE + ATA_REG_DATA, b->data, BSIZE / 4);
  b->disk = 0;
  wakeup(b);
  release(&ide.lock);
}


void
ideinit(void)
{
  initlock(&ide.lock, "ide");
  ioapicenable(IRQ_IDE, 0);
  // the BIOS may leave the slave selected; status reads 0 then.
  outb(IDE_BASE + ATA_REG_DEVICE, 0xE0);
  idewait(ATA_DRDY);
}
