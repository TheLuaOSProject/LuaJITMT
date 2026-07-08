#define _GNU_SOURCE
#include <execinfo.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void segv_bt_handler(int sig, siginfo_t *si, void *uctx)
{
  void *frames[64];
  int n;
  (void)uctx;
  fprintf(stderr, "\nsegv_bt: signal=%d addr=%p\n", sig, si ? si->si_addr : 0);
  n = backtrace(frames, (int)(sizeof(frames) / sizeof(frames[0])));
  backtrace_symbols_fd(frames, n, STDERR_FILENO);
  _exit(128 + sig);
}

__attribute__((constructor))
static void segv_bt_install(void)
{
  struct sigaction sa;
  sa.sa_sigaction = segv_bt_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
}
