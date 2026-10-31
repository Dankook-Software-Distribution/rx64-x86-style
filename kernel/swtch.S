# Context switch
#
#   void swtch(struct context *old, struct context *new);
#
# Save current registers in old. Load from new.
#
# x86_64: old in rdi, new in rsi. Saves the callee-saved registers,
# rsp and rip (riscv: ra, sp and s0-s11). The return address is
# popped into old->rip, and new resumes with jmp *new->rip.

.globl swtch
swtch:
        movq %rbx, 16(%rdi)
        movq %rbp, 24(%rdi)
        movq %r12, 32(%rdi)
        movq %r13, 40(%rdi)
        movq %r14, 48(%rdi)
        movq %r15, 56(%rdi)
        popq %rax
        movq %rax, 0(%rdi)
        movq %rsp, 8(%rdi)

        movq 16(%rsi), %rbx
        movq 24(%rsi), %rbp
        movq 32(%rsi), %r12
        movq 40(%rsi), %r13
        movq 48(%rsi), %r14
        movq 56(%rsi), %r15
        movq 8(%rsi), %rsp
        jmp *0(%rsi)
