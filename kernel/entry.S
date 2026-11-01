#include "kernel/x86.h"
#include "kernel/memlayout.h"
#include "kernel/param.h"

        # x86_64: qemu -kernel loads us as a Multiboot kernel
        # (riscv: qemu jumped to 0x80000000 in M-mode, 64-bit).
        # Flag bit 16 makes qemu use these physical addresses
        # instead of the 64-bit ELF headers. Must be in the first
        # 8KB; kernel.ld puts .multiboot first.
        .section .multiboot, "a"
        .align 4
mbhdr:
        .long 0x1BADB002                # magic
        .long 1<<16                     # flags: address fields valid
        .long -(0x1BADB002 + (1<<16))   # checksum: magic + flags + checksum == 0
        .long V2P_WO(mbhdr)             # header_addr: where this header lands
        .long V2P_WO(mbhdr)             # load_addr: start loading here
        .long V2P_WO(edata)             # load_end_addr: end of .data
        .long V2P_WO(end)               # bss_end_addr: end of .bss
        .long V2P_WO(_entry)            # entry_addr: jump here after loading

        # x86_64: Multiboot enters in 32-bit protected mode with
        # paging and interrupts off; ESP and GDTR are undefined.
        .text
        .code32
.globl _entry
_entry:
        # print 'A' on COM1 to show we got here. qemu's UART
        # needs no initialization to transmit.
        movw $COM1, %dx
        movb $'A', %al
        outb %al, %dx

        # x86_64: zero [edata, end); Multiboot loads only up to
        # edata (riscv: qemu loaded the whole ELF).
        cld
        movl $V2P_WO(edata), %edi
        xorl %eax, %eax
        movl $V2P_WO(end), %ecx
        subl $V2P_WO(edata), %ecx
        rep stosb

        # set up a stack for C, after the bss loop since it lives
        # in .bss. ebp = 0 ends the frame-pointer chain.
        movl $V2P_WO(bootstacktop), %esp
        xorl %ebp, %ebp

        # x86_64: switch to long mode (riscv: already 64-bit at boot).
        # PAE, cr3 and EFER.LME must all be set before CR0.PG.
        lgdt V2P_WO(bootgdtdesc)        # physical: paging is still off

        movl %cr4, %eax
        orl $CR4_PAE, %eax
        movl %eax, %cr4

        movl $V2P_WO(pml4), %eax
        movl %eax, %cr3

        # EFER is an MSR (number in ecx, value in edx:eax). NXE
        # makes PTE_XD usable.
        movl $MSR_EFER, %ecx
        rdmsr
        orl $(EFER_LME | EFER_NXE), %eax
        wrmsr

        movl %cr0, %eax
        orl $CR0_PG, %eax
        movl %eax, %cr0

        # CS is still 32-bit (compatibility mode); a far jump to
        # the L=1 code segment enters 64-bit mode, still low.
        ljmp $KCSEG, $V2P_WO(start64)

        .code64
start64:
        # the boot GDT has no data segment: in 64-bit mode the kernel
        # may load null selectors into ds/es/ss (Linux does the same).
        xorl %eax, %eax
        movw %ax, %ds
        movw %ax, %es
        movw %ax, %ss

        movw $COM1, %dx
        movb $'B', %al
        outb %al, %dx

        # x86_64: jump to the high address the kernel is linked at
        # (riscv: ran where linked). A plain jmp is RIP-relative and
        # would stay low, so go through a register.
        movabs $high64, %rax
        jmp *%rax

high64:
        movb $'C', %al
        outb %al, %dx

        # move the stack to its high address too (16-byte aligned
        # for the call); ebp = 0 ends the frame-pointer chain.
        movabs $bootstacktop, %rsp
        xorl %ebp, %ebp

        # main should not return; if it does, fall into spin.
        call main

spin:
        hlt
        jmp spin

        # x86_64: stack for the boot CPU only (riscv: stack0, one
        # per CPU); it stays CPU 0's scheduler stack.
        .bss
        .balign KSTACKSIZE
bootstack:
        .space KSTACKSIZE
bootstacktop:

        # x86_64: boot page table, used only until kvminithart()
        # (riscv: paging stayed off until then). Maps physical
        # 0..1GB twice with 2MB pages:
        #   VA 0         -> PA 0   identity, while still running low
        #   VA KERNBASE  -> PA 0   where the kernel is linked
        # In .data, not .bss: Multiboot loads only up to edata.
        .data
        .balign 4096
.globl pml4                             # entryother.S uses it too
pml4:
        .quad V2P_WO(pdpt) + PTE_P + PTE_RW     # [0]   VA 0 .. 512GB
        .space (511 - 1) * 8                    # [1..510] not present
        .quad V2P_WO(pdpt) + PTE_P + PTE_RW     # [511] top 512GB, holds KERNBASE

        .balign 4096
pdpt:
        .quad V2P_WO(pd) + PTE_P + PTE_RW       # [0]   VA 0 .. 1GB
        .space (511 - 2) * 8                    # [1..509] not present
        .quad V2P_WO(pd) + PTE_P + PTE_RW       # [510] KERNBASE .. +1GB
        .space 8                                # [511] not present

        .balign 4096
pd:
        # 512 2MB pages: entry i maps PA i * 2MB.
        .set pd_i, 0
        .rept 512
        .quad (pd_i << 21) + PTE_PS + PTE_P + PTE_RW
        .set pd_i, pd_i + 1
        .endr

        # x86_64: boot GDT, just a 64-bit code segment for the far
        # jump, in slot 1 as in seginit()'s GDT so KCSEG matches.
        .data
        .balign 8
bootgdt:
        .quad 0                         # 0x00: null (required)
        .quad 0x00209A0000000000        # 0x08: KCSEG, 64-bit kernel code
bootgdt_end:

bootgdtdesc:
        .word bootgdt_end - bootgdt - 1 # limit: size of GDT - 1
        .long V2P_WO(bootgdt)           # base: physical, paging is off
