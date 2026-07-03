/*
** Focused regression test for cdata get/set conversion ctype snapshots.
*/

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

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

static void assert_cdata_conversion_waits(lua_State *L, CTState *cts,
					  TGState *tg, const char *chunk)
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

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "jit.off()\n"
    "ffi.cdef[[\n"
    "typedef enum { LJ_M7_CDATA_CONV_A = 7 } lj_m7_cdata_conv_enum_t;\n"
    "typedef struct { int x; } lj_m7_cdata_conv_node_t;\n"
    "typedef struct {\n"
    "  lj_m7_cdata_conv_enum_t e;\n"
    "  lj_m7_cdata_conv_node_t *p;\n"
    "} lj_m7_cdata_conv_box_t;\n"
    "]]\n"
    "lj_m7_cdata_conv_box = ffi.new('lj_m7_cdata_conv_box_t[1]')\n"
    "lj_m7_cdata_conv_node = ffi.new('lj_m7_cdata_conv_node_t[1]')\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  tg = L2TG(L);
  assert(tg != NULL);

  assert_cdata_conversion_waits(L, cts, tg,
    "lj_m7_cdata_conv_box[0].e = 'LJ_M7_CDATA_CONV_A'\n"
    "assert(tonumber(lj_m7_cdata_conv_box[0].e) == 7)\n");

  assert_cdata_conversion_waits(L, cts, tg,
    "lj_m7_cdata_conv_node[0].x = 99\n"
    "lj_m7_cdata_conv_box[0].p = lj_m7_cdata_conv_node\n"
    "assert(lj_m7_cdata_conv_box[0].p[0].x == 99)\n");

  lua_close(L);
  printf("t-ffi-cdata-conv-snapshot OK: cdata conversions wait on ctype snapshots\n");
  return 0;
}
