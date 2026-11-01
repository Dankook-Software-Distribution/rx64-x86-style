//
// low-level driver for 16550a UART.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"

// x86_64: COM1's registers are I/O ports (riscv: memory-mapped
// at UART0), reached with inb/outb.
#define ReadReg(reg)     inb(COM1 + (reg))
#define WriteReg(reg, v) outb(COM1 + (reg), (v))

// the UART control registers, named as in Linux
// include/uapi/linux/serial_reg.h.
// some have different meanings for read vs write.
#define UART_RX       0    // receive buffer (read)
#define UART_TX       0    // transmit holding (write)
#define UART_DLL      0    // divisor latch low (with DLAB)
#define UART_DLM      1    // divisor latch high (with DLAB)
#define UART_IER      1    // interrupt enable
#define UART_IER_RDI  0x01 // receiver data interrupt
#define UART_IER_THRI 0x02 // transmit holding register empty
#define UART_IIR      2    // interrupt identification (read)
#define UART_FCR      2    // FIFO control (write)
#define UART_FCR_ENABLE_FIFO 0x01
#define UART_FCR_CLEAR_RCVR  0x02
#define UART_FCR_CLEAR_XMIT  0x04
#define UART_LCR      3    // line control
#define UART_LCR_WLEN8 0x03 // 8 data bits
#define UART_LCR_DLAB 0x80 // divisor latch access
#define UART_MCR      4    // modem control
#define UART_MCR_DTR  0x01
#define UART_MCR_RTS  0x02
#define UART_MCR_OUT2 0x08 // PC wiring: gates the IRQ line
#define UART_LSR      5    // line status
#define UART_LSR_DR   0x01 // data ready
#define UART_LSR_THRE 0x20 // transmit holding register empty

// for sending threads to serialize their writes
static struct sleeplock tx_lock;
static int tx_chan; // &tx_chan is the "wait channel"

extern volatile int panicking; // from printk.c
extern volatile int panicked;  // from printk.c

void
uartinit(void)
{
  // disable interrupts.
  WriteReg(UART_IER, 0x00);

  // special mode to set baud rate.
  WriteReg(UART_LCR, UART_LCR_DLAB);

  // LSB for baud rate of 38.4K.
  WriteReg(UART_DLL, 0x03);

  // MSB for baud rate of 38.4K.
  WriteReg(UART_DLM, 0x00);

  // leave set-baud mode,
  // and set word length to 8 bits, no parity.
  WriteReg(UART_LCR, UART_LCR_WLEN8);

  // reset and enable FIFOs.
  WriteReg(UART_FCR, UART_FCR_ENABLE_FIFO | UART_FCR_CLEAR_RCVR |
                       UART_FCR_CLEAR_XMIT);

  // x86_64: on a PC, OUT2 connects the UART's interrupt to the
  // interrupt controller (riscv's UART had no such gate).
  WriteReg(UART_MCR, UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT2);

  // enable transmit and receive interrupts.
  WriteReg(UART_IER, UART_IER_THRI | UART_IER_RDI);

  initsleeplock(&tx_lock, "uart");

  // x86_64: route IRQ 4 to CPU 0 (riscv: the PLIC, in plicinit()).
  ioapicenable(IRQ_COM1, 0);
}

// transmit buf[] to the uart. it blocks if the
// uart is busy, so it cannot be called from
// interrupts, only from write() system calls.
void
uartwrite(char buf[], int n)
{
  acquiresleep(&tx_lock);

  int i = 0;
  while (i < n) {
    sleep_prepare(&tx_chan);
    if (ReadReg(UART_LSR) & UART_LSR_THRE) {
      WriteReg(UART_TX, buf[i]);
      i += 1;
    } else {
      sleep();
    }
  }

  releasesleep(&tx_lock);
}

// write a byte to the uart without using
// interrupts, for use by kernel printk() and
// to echo characters. it spins waiting for the uart's
// output register to be empty.
void
uartputc_sync(int c)
{
  if (panicking == 0)
    push_off();

  if (panicked) {
    for (;;)
      ;
  }

  // wait for UART to set Transmit Holding Empty in LSR.
  while ((ReadReg(UART_LSR) & UART_LSR_THRE) == 0)
    ;
  WriteReg(UART_TX, c);

  if (panicking == 0)
    pop_off();
}

// try to read one input character from the UART.
// return -1 if none is waiting.
static int
uartgetc(void)
{
  // is input ready?
  if (ReadReg(UART_LSR) & UART_LSR_DR) {
    return ReadReg(UART_RX);
  } else {
    return -1;
  }
}

// handle a uart interrupt, raised because input has
// arrived, or the uart is ready for more output, or
// both. called from trap().
void
uartintr(void)
{
  ReadReg(UART_IIR); // acknowledge the interrupt

  if (ReadReg(UART_LSR) & UART_LSR_THRE) {
    // UART finished transmitting; wake up sending thread.
    wakeup(&tx_chan);
  }

  // read and process incoming characters, if any.
  while (1) {
    int c = uartgetc();
    if (c == -1)
      break;
    consoleintr(c);
  }
}
