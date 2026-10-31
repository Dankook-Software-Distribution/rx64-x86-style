# x86_64: one entry stub per interrupt vector (riscv: a single
# kernelvec; scause told what happened). Each stub pushes its
# vector number, after a fake 0 error code where the CPU pushes
# none. Made with .rept, as Linux does, not xv6's vectors.pl.

#include "kernel/traps.h"

# stub for vector n. The CPU pushes an error code only for these
# exceptions (SDM Vol. 3A Table 6-1); see the "err" marks in
# traps.h.
.macro vector n
vector\n:
        .if !(\n == T_DF || (\n >= T_TS && \n <= T_PF) || \
              \n == T_AC || \n == T_CP)
                pushq $0                # fake error code
        .endif
        pushq $\n                       # trapno
        jmp alltraps
.endm

# one table entry: vector n's address, filled in by the linker.
.macro vecptr n
        .quad vector\n
.endm

        # vector0 .. vector255. In .altmacro mode, %i passes i's
        # value rather than the letter i.
        .text
        .altmacro
        .set i, 0
        .rept 256
                vector %i
                .set i, i + 1
        .endr

        # vectors[n] is the address of vectorn, for idtinit().
        # The stubs differ in size, so they need a table.
        .section .rodata
        .balign 8
.globl vectors
vectors:
        .set i, 0
        .rept 256
                vecptr %i
                .set i, i + 1
        .endr
        .noaltmacro
