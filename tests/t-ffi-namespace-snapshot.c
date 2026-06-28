/*
** Focused guard for lock-free ffi.C namespace snapshots.
*/

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
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

static void *release_parse_token(void *arg)
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

static void assert_namespace_waits_without_lock(lua_State *L, CTState *cts,
						TGState *tg,
						const char *chunk)
{
  ParseReleaseCtx ctx;
  pthread_t thread;
  uint32_t seq0 = ljt_ctype_parse_seq(cts);

  ctx.cts = cts;
  ctx.tg = tg;
  ctx.release_seq = ljt_ctype_hold_parse_token(cts);
  ctx.saw_native = 0;
  assert(ctx.release_seq == seq0 + 2u);

  assert(pthread_create(&thread, NULL, release_parse_token, &ctx) == 0);
  ljt_lua_dostring(L, chunk);
  assert(pthread_join(thread, NULL) == 0);
  assert(ctx.saw_native);
  assert(ljt_ctype_parse_seq(cts) == ctx.release_seq);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  CTState *cts;
  TGState *tg;
  uint32_t seq0, seq1, seq2, seq3;

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "int abs(int);\n"
    "unsigned long lj_m7_ns_strlen(const char *) asm(\"strlen\");\n"
    "enum { LJ_M7_NS_CONST = 91 };\n"
    "int getpid(void);\n"
    "unsigned long lj_m7_ns_wait_strlen(const char *) asm(\"strlen\");\n"
    "enum { LJ_M7_NS_WAIT_CONST = 123 };\n"
    "]]\n"
    "jit.off()\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  tg = L2TG(L);
  assert(tg != NULL);
  seq0 = ljt_ctype_parse_seq(cts);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "assert(ffi.C.LJ_M7_NS_CONST == 91)\n"
    "assert(ffi.C.abs(-44) == 44)\n"
    "assert(tonumber(ffi.C.lj_m7_ns_strlen('abcd')) == 4)\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0);

  assert_namespace_waits_without_lock(L, cts, tg,
    "local ffi = require('ffi')\n"
    "assert(ffi.C.LJ_M7_NS_WAIT_CONST == 123)\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 2u);

  assert_namespace_waits_without_lock(L, cts, tg,
    "local ffi = require('ffi')\n"
    "assert(ffi.C.getpid() > 0)\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 4u);

  assert_namespace_waits_without_lock(L, cts, tg,
    "local ffi = require('ffi')\n"
    "assert(tonumber(ffi.C.lj_m7_ns_wait_strlen('abcde')) == 5)\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0 + 6u);

  {
    ljt_ctype_arm_trace_abort(L, cts);
    ljt_lua_dostring(L,
      "local ffi = require('ffi')\n"
      "local C = ffi.C\n"
      "jit.attach(lj_m7_trace_parse_token, 'trace')\n"
      "jit.flush()\n"
      "jit.on()\n"
      "jit.opt.start('hotloop=1', 'hotexit=1')\n"
      "local function run(n)\n"
      "  local sum = 0\n"
      "  for i = 1, n do\n"
      "    sum = sum + C.LJ_M7_NS_CONST + C.abs(-i)\n"
      "  end\n"
      "  return sum\n"
      "end\n"
      "for i = 1, 3 do assert(run(8) == 764) end\n"
      "jit.attach(lj_m7_trace_parse_token)\n"
      "assert(lj_m7_trace_parse_token_abort_count() >= 1)\n");
    ljt_ctype_assert_trace_abort_released(cts);
  }
  seq2 = ljt_ctype_parse_seq(cts);
  assert(seq2 == seq1 + 2u);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local C = ffi.C\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function run(n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do\n"
    "    sum = sum + C.LJ_M7_NS_CONST + C.abs(-i)\n"
    "  end\n"
    "  return sum\n"
    "end\n"
    "for i = 1, 30 do assert(run(40) == 4460) end\n");
  seq3 = ljt_ctype_parse_seq(cts);
  assert(seq3 == seq2);

  lua_close(L);
  printf("t-ffi-namespace-snapshot OK: stable ffi.C lookups avoid cparser sequence\n");
  return 0;
}
