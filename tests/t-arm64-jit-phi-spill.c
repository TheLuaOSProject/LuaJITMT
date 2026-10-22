/* Execute the production PHI spill-copy boundary with trace admission closed. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include "lua.h"
#include "lauxlib.h"

#if defined(__APPLE__) && defined(__aarch64__) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && \
    defined(LJ_ARM64_EMIT_TEST_HELPERS)
#include "lj_obj.h"
#include "lj_asm.h"
#include "lj_dispatch.h"
#include "lj_jit.h"
#include "lj_mcode.h"

extern void lj_test_arm64_phi_spill_call(MCode *code, double *values,
                                        uint32_t iterations);

int main(int argc, char **argv)
{
  lua_State *L = luaL_newstate();
  size_t pagesz = (size_t)sysconf(_SC_PAGESIZE);
  MCode *area, *code;
  MSize cap;
  uint32_t n;
  FILE *out;
  assert(L != NULL && argc == 2 && pagesz >= 4096u);
  area = mmap(NULL, pagesz, PROT_READ|PROT_WRITE|PROT_EXEC,
              MAP_PRIVATE|MAP_ANON|MAP_JIT, -1, 0);
  assert(area != MAP_FAILED);
  cap = (MSize)(pagesz/sizeof(MCode));
  if (pthread_jit_write_protect_supported_np())
    pthread_jit_write_protect_np(0);
  code = lj_asm_arm64_phi_spill_test(L2J(L), area, cap);
  assert(code != NULL && code >= area && code < area+cap);
  lj_mcode_sync(code, area+cap);
  if (pthread_jit_write_protect_supported_np())
    pthread_jit_write_protect_np(1);
  out = fopen(argv[1], "wb");
  assert(out != NULL);
  assert(fwrite(code, sizeof(*code), (size_t)(area+cap-code), out) ==
         (size_t)(area+cap-code));
  assert(fclose(out) == 0);
  for (n = 1; n <= 100; n++) {
    double values[] = { 123.5, 456.5, 789.5, 321.5, -1.0, -1.0 };
    lj_test_arm64_phi_spill_call(code, values, n);
    /* First entry bypasses the copies; only a backedge may install the RHS. */
    assert(values[4] == (n == 1 ? values[3] : values[2]));
    assert(values[5] == values[1]);
  }
  assert(munmap(area, pagesz) == 0);
  lua_close(L);
  puts("t-arm64-jit-phi-spill OK: emitted prologue bypass and PHI backedge copies");
  return 0;
}
#else
int main(void)
{
  puts("t-arm64-jit-phi-spill SKIP");
  return 0;
}
#endif
