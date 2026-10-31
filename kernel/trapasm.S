        # x86_64: common trap entry and exit (riscv: kernelvec, and
        # uservec/userret in trampoline.S). Every stub in vectors.S
        # jumps to alltraps with the top of a trapframe pushed:
        #
        #   ss, rsp, rflags, cs, rip    pushed by the CPU
        #   err                         by the CPU, or a fake 0 by the stub
        #   trapno                      by the stub   <- rsp
        #
        # alltraps pushes the rest and calls trap(tf); trapret
        # undoes it with iretq.

        .text
.globl alltraps
alltraps:
        # struct trapframe starts with rax at offset 0, and the
        # stack grows down, so push in the reverse of field order:
        # r15 first, rax last.
        pushq %r15
        pushq %r14
        pushq %r13
        pushq %r12
        pushq %r11
        pushq %r10
        pushq %r9
        pushq %r8
        pushq %rdi
        pushq %rsi
        pushq %rbp
        pushq %rdx
        pushq %rcx
        pushq %rbx
        pushq %rax

        # pass the trapframe. rsp is 16-byte aligned: the CPU
        # aligns it, and 176 is a multiple of 16.
        movq %rsp, %rdi
        call trap

        # trap() returns here. A new process will also start here
        # on its first return to user space.
.globl trapret
trapret:
        popq %rax
        popq %rbx
        popq %rcx
        popq %rdx
        popq %rbp
        popq %rsi
        popq %rdi
        popq %r8
        popq %r9
        popq %r10
        popq %r11
        popq %r12
        popq %r13
        popq %r14
        popq %r15

        # drop trapno and err, leaving rsp at the CPU's frame.
        addq $16, %rsp

        # pop rip, cs, rflags, rsp and ss (iretq: 8-byte values).
        iretq
