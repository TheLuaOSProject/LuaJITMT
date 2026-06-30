/*
** Focused guard for ccall small-struct stack overflow ctype snapshots.
*/

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_arch.h"
#include "lj_obj.h"
#include "lj_ctype.h"
#include "lj_tg.h"

#include "lib/ctype_parse_fixture_helpers.h"
#include "lib/lua_fixture_helpers.h"

typedef struct ParseReleaseCtx {
  CTState *cts;
  TGState *tg;
  uint32_t release_seq;
  int saw_native;
} ParseReleaseCtx;

static void sleep_ns(long ns)
{
  struct timespec ts;
  ts.tv_sec = ns / 1000000000l;
  ts.tv_nsec = ns % 1000000000l;
  while (nanosleep(&ts, &ts) != 0)
    ;
}

static void *release_parse_token_when_native(void *arg)
{
  ParseReleaseCtx *ctx = (ParseReleaseCtx *)arg;
  int spins;
  for (spins = 0; spins < 1000; spins++) {
    if (lj_tg_in_native_acq(ctx->tg)) {
      ctx->saw_native = 1;
      break;
    }
    sleep_ns(1000000);
  }
  ljt_ctype_release_parse_token(ctx->cts, ctx->release_seq);
  return NULL;
}

#if LJ_TARGET_X64 && !LJ_ABI_WIN
static void assert_ccall_struct_overflow_waits(lua_State *L, CTState *cts,
					       TGState *tg)
{
  ParseReleaseCtx ctx;
  pthread_t thread;
  uint32_t seq0 = ljt_ctype_parse_seq(cts);

  ctx.cts = cts;
  ctx.tg = tg;
  ctx.release_seq = ljt_ctype_hold_parse_token(cts);
  ctx.saw_native = 0;
  assert(ctx.release_seq == seq0 + 2u);

  assert(pthread_create(&thread, NULL, release_parse_token_when_native,
			&ctx) == 0);
  ljt_lua_dostring(L,
    "local s = { x = 37 }\n"
    "local r = lj_m7_ccall_struct_lib.lj_m7_ccall_struct_overflow(\n"
    "  1, 2, 3, 4, 5, 6, s)\n"
    "assert(r == 58)\n");
  assert(pthread_join(thread, NULL) == 0);
  assert(ctx.saw_native);
  assert(ljt_ctype_parse_seq(cts) == ctx.release_seq);
}
#endif

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  CTState *cts;
  TGState *tg;

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "jit.off()\n"
    "ffi.cdef[[\n"
    "typedef struct { int x; } lj_m7_ccall_struct_overflow_t;\n"
    "int lj_m7_ccall_struct_overflow(int, int, int, int, int, int,\n"
    "                                lj_m7_ccall_struct_overflow_t);\n"
    "]]\n"
    "lj_m7_ccall_struct_lib = ffi.load(\n"
    "  assert(os.getenv('LJ_M7_FFI_CCALL_STRUCT_SO')))\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  tg = L2TG(L);
  assert(tg != NULL);

#if LJ_TARGET_X64 && !LJ_ABI_WIN
  assert_ccall_struct_overflow_waits(L, cts, tg);
#endif

  lua_close(L);
  printf("t-ffi-ccall-struct-overflow OK: ccall struct overflow conversion waits on snapshots\n");
  return 0;
}
