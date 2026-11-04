# x86_64: startup code for the other CPUs (APs), like Linux's
# realmode trampoline (riscv: every hart entered _entry at once).
# An AP wakes from INIT-SIPI in 16-bit real mode at ENTRYOTHER,
# where startothers() copies these bytes. It climbs to 32-bit and
# then 64-bit mode on entry.S's boot page table, takes the stack
# and entry point startothers() left just below ENTRYOTHER, and
# calls mpenter() at the kernel's high address.

#include "memlayout.h"
#include "x86.h"

# where a label ends up once the code is copied to ENTRYOTHER.
#define ABS(x) ((x) - entryother + ENTRYOTHER)

#define TCODE32 0x10                    # tgdt's 32-bit code segment

        .section .rodata                # copied, never run in place
.globl entryother
entryother:
        .code16
        cli
        xorw %ax, %ax
        movw %ax, %ds
        lgdtl ABS(tgdtdesc)
        movl %cr0, %eax
        orl $CR0_PE, %eax
        movl %eax, %cr0
        # ljmpl $TCODE32, $ABS(start32). gas takes no label
        # arithmetic in a far jump, so spell the bytes out.
        .byte 0x66, 0xea
        .long ABS(start32)
        .word TCODE32

        .code32
start32:
        # the same steps as entry.S, on its boot page table.
        movl %cr4, %eax
        orl $CR4_PAE, %eax
        movl %eax, %cr4

        movl $V2P_WO(pml4), %eax
        movl %eax, %cr3

        movl $MSR_EFER, %ecx
        rdmsr
        orl $(EFER_LME | EFER_NXE), %eax
        wrmsr

        # x86_64: WP with PG, as Linux; without it kernel writes
        # ignore read-only PTEs (riscv has no such bypass).
        # Clear CD and NW, which INIT sets, to turn caching on.
        movl %cr0, %eax
        orl $(CR0_PG | CR0_WP), %eax
        andl $~(CR0_CD | CR0_NW), %eax
        movl %eax, %cr0

        # ljmp $KCSEG, $ABS(start64), spelled out as above.
        .byte 0xea
        .long ABS(start64)
        .word KCSEG

        .code64
start64:
        xorl %eax, %eax
        movw %ax, %ds
        movw %ax, %es
        movw %ax, %ss

        movq ENTRYOTHER - 8, %rsp       # stack top, from startothers()
        movq ENTRYOTHER - 16, %rax      # mpenter()
        xorl %ebp, %ebp
        call *%rax                      # a call leaves rsp % 16 == 8

spin:
        hlt
        jmp spin

        # 64-bit code sits in slot 1 as in the kernel GDT, so
        # seginit() need not reload CS.
        .p2align 3
tgdt:
        .quad 0                         # 0x00: null
        .quad 0x00209A0000000000        # 0x08: KCSEG, 64-bit code
        .quad 0x00CF9A000000FFFF        # 0x10: 32-bit code, base 0, 4GB
tgdtdesc:
        .word tgdtdesc - tgdt - 1
        .long ABS(tgdt)

.globl entryother_end
entryother_end:
