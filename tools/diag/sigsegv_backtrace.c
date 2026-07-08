#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>
#include <unistd.h>

static void segv_handler(int sig, siginfo_t *si, void *uctx)
{
  void *bt[64];
  int n, i;
  uintptr_t rip = 0;
#if defined(__x86_64__) && defined(REG_RIP)
  ucontext_t *uc = (ucontext_t *)uctx;
  rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
  dprintf(STDERR_FILENO,
	  "regs: rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx "
	  "rsi=0x%llx rdi=0x%llx r8=0x%llx r9=0x%llx r10=0x%llx "
	  "r11=0x%llx r12=0x%llx r13=0x%llx r14=0x%llx r15=0x%llx "
	  "rsp=0x%llx rbp=0x%llx\n",
	  (unsigned long long)uc->uc_mcontext.gregs[REG_RAX],
	  (unsigned long long)uc->uc_mcontext.gregs[REG_RBX],
	  (unsigned long long)uc->uc_mcontext.gregs[REG_RCX],
	  (unsigned long long)uc->uc_mcontext.gregs[REG_RDX],
	  (unsigned long long)uc->uc_mcontext.gregs[REG_RSI],
	  (unsigned long long)uc->uc_mcontext.gregs[REG_RDI],
	  (unsigned long long)uc->uc_mcontext.gregs[REG_R8],
	  (unsigned long long)uc->uc_mcontext.gregs[REG_R9],
	  (unsigned long long)uc->uc_mcontext.gregs[REG_R10],
	  (unsigned long long)uc->uc_mcontext.gregs[REG_R11],
	  (unsigned long long)uc->uc_mcontext.gregs[REG_R12],
	  (unsigned long long)uc->uc_mcontext.gregs[REG_R13],
	  (unsigned long long)uc->uc_mcontext.gregs[REG_R14],
	  (unsigned long long)uc->uc_mcontext.gregs[REG_R15],
	  (unsigned long long)uc->uc_mcontext.gregs[REG_RSP],
	  (unsigned long long)uc->uc_mcontext.gregs[REG_RBP]);
#else
  (void)uctx;
#endif
  dprintf(STDERR_FILENO, "sigsegv_backtrace: signal=%d addr=%p rip=0x%lx\n",
	  sig, si ? si->si_addr : NULL, (unsigned long)rip);
  n = backtrace(bt, (int)(sizeof(bt) / sizeof(bt[0])));
  for (i = 0; i < n; i++) {
    Dl_info di;
    if (dladdr(bt[i], &di) && di.dli_sname) {
      dprintf(STDERR_FILENO, "#%02d %p %s+0x%lx (%s)\n", i, bt[i],
	      di.dli_sname,
	      (unsigned long)((uintptr_t)bt[i] - (uintptr_t)di.dli_saddr),
	      di.dli_fname ? di.dli_fname : "?");
    } else {
      dprintf(STDERR_FILENO, "#%02d %p\n", i, bt[i]);
    }
  }
  _exit(128 + sig);
}

__attribute__((constructor))
static void install_segv_handler(void)
{
  struct sigaction sa;
  sa.sa_sigaction = segv_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
}
