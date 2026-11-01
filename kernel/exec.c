#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "x86.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

static int loadseg(pde_t *, uint64, struct inode *, uint, uint);

// map ELF permissions to PTE permission bits.
// x86_64: XD unless executable (riscv: X if executable), and
// uint64 since XD is bit 63.
uint64
flags2perm(int flags)
{
  uint64 perm = 0;
  if (!(flags & ELF_PROG_FLAG_EXEC))
    perm = PTE_XD;
  if (flags & ELF_PROG_FLAG_WRITE)
    perm |= PTE_RW;
  return perm;
}

//
// the implementation of the exec() system call
//
int
kexec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();

  begin_op();

  // Open the executable file.
  if ((ip = namei(path)) == 0) {
    end_op();
    return -1;
  }
  ilock(ip);

  // Read the ELF header.
  if (readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;

  // Is this really an ELF file?
  if (elf.magic != ELF_MAGIC)
    goto bad;
  if (elf.machine != EM_X86_64) // x86_64: riscv did not check
    goto bad;

  if ((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  // Load program into memory.
  for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)) {
    if (readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if (ph.type != ELF_PROG_LOAD)
      continue;
    if (ph.memsz < ph.filesz)
      goto bad;
    if (ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if (ph.vaddr % PGSIZE != 0)
      goto bad;
    uint64 sz1;
    if ((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz,
                        flags2perm(ph.flags))) == 0)
      goto bad;
    sz = sz1;
    if (loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  p = myproc();
  uint64 oldsz = p->sz;

  // Allocate some pages at the next page boundary.
  // Make the first inaccessible as a stack guard.
  // Use the rest as the user stack.
  sz = PGROUNDUP(sz);
  uint64 sz1;
  // x86_64: XD, since x86 pages are executable unless marked.
  if ((sz1 = uvmalloc(pagetable, sz, sz + (USERSTACK + 1) * PGSIZE,
                      PTE_RW | PTE_XD)) == 0)
    goto bad;
  sz = sz1;
  uvmclear(pagetable, sz - (USERSTACK + 1) * PGSIZE);
  sp = sz;
  stackbase = sp - USERSTACK * PGSIZE;

  // Copy argument strings into new stack, remember their
  // addresses in ustack[].
  for (argc = 0; argv[argc]; argc++) {
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // the x86_64 ABI wants 16-byte alignment too
    if (sp < stackbase)
      goto bad;
    if (copyout(pagetable, sz, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // push a copy of ustack[], the array of argv[] pointers.
  sp -= (argc + 1) * sizeof(uint64);
  sp -= sp % 16;
  if (sp < stackbase)
    goto bad;
  if (copyout(pagetable, sz, sp, (char *)ustack, (argc + 1) * sizeof(uint64)) <
      0)
    goto bad;

  // x86_64: fake return address, so that start() sees the stack
  // a call would leave (rsp % 16 == 8). riscv kept it in ra.
  uint64 fakepc = ~0UL;
  sp -= sizeof(uint64);
  if (sp < stackbase)
    goto bad;
  if (copyout(pagetable, sz, sp, (char *)&fakepc, sizeof(uint64)) < 0)
    goto bad;

  // x86_64: arguments to start(argc, argv) in rdi and rsi. riscv
  // returned argc into a0, but x86 returns in rax, not rdi.
  p->trapframe->rdi = argc;
  p->trapframe->rsi = sp + sizeof(uint64);

  // Save program name for debugging.
  for (last = s = path; *s; s++)
    if (*s == '/')
      last = s + 1;
  safestrcpy(p->name, last, sizeof(p->name));

  // Commit to the user image.
  oldpagetable = p->pagetable;
  p->pagetable = pagetable;
  p->sz = sz;
  p->trapframe->rip = elf.entry; // initial program counter = ulib.c:start()
  p->trapframe->rsp = sp;        // initial stack pointer
  // x86_64: fresh user segments and rflags, as Linux start_thread();
  // allocproc() zeroed them for the first process.
  p->trapframe->cs = UCSEG | 3;
  p->trapframe->ss = UDSEG | 3;
  p->trapframe->rflags = FL_IF;
  // x86_64: cr3 still holds the old table; switch before freeing
  // it (riscv ran on the kernel table until userret).
  write_cr3(V2P(pagetable));
  proc_freepagetable(oldpagetable, oldsz);

  return argc; // x86_64: lands in rax; start() reads rdi

bad:
  if (pagetable)
    proc_freepagetable(pagetable, sz);
  if (ip) {
    iunlockput(ip);
    end_op();
  }
  return -1;
}

// Load an ELF program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
static int
loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset,
        uint sz)
{
  uint i, n;
  uint64 pa;

  for (i = 0; i < sz; i += PGSIZE) {
    pa = walkaddr(pagetable, va + i);
    if (pa == 0)
      panic("loadseg: address should exist");
    if (sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    // x86_64: walkaddr() gives a PA; write through the kernel map.
    if (readi(ip, 0, (uint64)P2V(pa), offset + i, n) != n)
      return -1;
  }

  return 0;
}
