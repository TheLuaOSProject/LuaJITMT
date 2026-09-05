/* Diagnostic only: observe after the program has already chosen abort(). */
#include <execinfo.h>
#include <unistd.h>
#include <stdlib.h>
extern void __real_abort(void) __attribute__((noreturn));
void __wrap_abort(void)
{
  void *frames[64];
  int n;
  static const char tag[] = "ABORT_OBSERVER\n";
  (void)write(STDERR_FILENO, tag, sizeof(tag)-1u);
  n = backtrace(frames, 64);
  backtrace_symbols_fd(frames, n, STDERR_FILENO);
  __real_abort();
}
