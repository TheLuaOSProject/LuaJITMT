#define _GNU_SOURCE 1
#include <stdint.h>
#include <signal.h>
#include <sys/prctl.h>
#include <unistd.h>
extern void __real_abort(void) __attribute__((noreturn));
uintptr_t diag_abort_rdi;
__attribute__((noreturn, noinline)) void diag_abort_observe(uintptr_t raw_rdi)
{
  diag_abort_rdi = raw_rdi;
  (void)prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
  (void)raise(SIGSTOP);
  __real_abort();
}
__attribute__((naked,noreturn)) void __wrap_abort(void)
{
  __asm__ volatile("jmp diag_abort_observe");
}
