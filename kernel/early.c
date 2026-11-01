// x86_64: early console output (riscv: none), like Linux's
// early_printk: straight to COM1 with no lock, so it also works
// in trap handlers and panics. No wait for LSR.THRE; qemu does
// not need it.

#include "types.h"
#include "x86.h"
#include "memlayout.h"

static char digits[] = "0123456789abcdef";

// print one character.
void
earlyputc(int c)
{
  outb(COM1, c);
}

// print a NUL-terminated string.
void
earlyputs(char *s)
{
  while (*s) {
    earlyputc(*s);
    s++;
  }
}

// print x as 0x and 16 hex digits, as printptr() does.
void
earlyputx(uint64 x)
{
  earlyputs("0x");
  for (int i = 0; i <= 15; i++) {
    outb(COM1, digits[x >> 60]);
    x = x << 4;
  }
}
