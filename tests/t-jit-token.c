/*
** Focused guard for the M6 JIT recorder token.
*/

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_jit.h"
#include "lj_safepoint.h"
#include "lj_trace.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_target.h"

#include "lib/lua_fixture_helpers.h"

typedef struct TokenReleaseCtx {
  global_State *g;
  uint32_t owner;
  uint32_t released;
} TokenReleaseCtx;

typedef struct TokenStopReqCtx {
  global_State *g;
  TGState *tg;
  uint32_t owner;
  uint32_t saw_native;
  uint32_t signaled;
  uint32_t released;
} TokenStopReqCtx;

static uint32_t foreign_token_owner(lua_State *L)
{
  uint32_t self = lj_tg_tid_acq(L2TG(L));
  return self == 0x7fffffffu ? 0x7ffffffeu : 0x7fffffffu;
}

static void *release_jit_token_after_delay(void *arg)
{
  TokenReleaseCtx *ctx = (TokenReleaseCtx *)arg;
  (void)lj_thr_sleep_ns(NULL, 30000000);
  assert(jit_token_acq(ctx->g) == ctx->owner);
  la_store32_rel(&ctx->released, 1);
  jit_token_rel(ctx->g, 0);
  return NULL;
}

static void *publish_stopreq_while_token_waits(void *arg)
{
  TokenStopReqCtx *ctx = (TokenStopReqCtx *)arg;
  int i;
  for (i = 0; i < 5000; i++) {
    if (lj_tg_in_native_acq(ctx->tg)) {
      la_store32_rel(&ctx->saw_native, 1);
      break;
    }
    (void)lj_thr_sleep_ns(NULL, 1000000);
  }
  assert(la_load32_acq(&ctx->saw_native) != 0);
  la_store32_rel(&ctx->signaled,
		 lj_safepoint_handshake(ctx->g, LJ_GC2_HS_STOPREQ));
  assert(jit_token_acq(ctx->g) == ctx->owner);
  jit_token_rel(ctx->g, 0);
  la_store32_rel(&ctx->released, 1);
  return NULL;
}

static void make_token_flush_trace(lua_State *L)
{
  ljt_lua_dostring(L,
    "local util = require'jit.util'\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "function lj_m6_token_tracecount()\n"
    "  local n = 0\n"
    "  for i = 1, 32 do if util.traceinfo(i) then n = n + 1 end end\n"
    "  return n\n"
    "end\n"
    "jit.off(lj_m6_token_tracecount, true)\n"
    "function lj_m6_token_flush_f(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i end\n"
    "  return s\n"
    "end\n"
    "for _ = 1, 20 do assert(lj_m6_token_flush_f(80) == 3240) end\n"
    "assert(util.traceinfo(1), 'expected trace 1')\n"
    "assert(lj_m6_token_tracecount() > 0)\n");
}

static void expect_flush_waits_for_token(lua_State *L, const char *code)
{
  TokenReleaseCtx ctx;
  pthread_t th;
  global_State *g = G(L);
  make_token_flush_trace(L);
  ctx.g = g;
  ctx.owner = foreign_token_owner(L);
  ctx.released = 0;
  jit_token_rel(g, ctx.owner);
  assert(pthread_create(&th, NULL, release_jit_token_after_delay, &ctx) == 0);
  ljt_lua_dostring(L, code);
  assert(pthread_join(th, NULL) == 0);
  assert(la_load32_acq(&ctx.released) == 1);
  assert(jit_token_acq(g) == 0);
  ljt_lua_dostring(L, "assert(lj_m6_token_tracecount() == 0)\n");
}

static void expect_opt_start_waits_for_token(lua_State *L)
{
  TokenReleaseCtx ctx;
  pthread_t th;
  global_State *g = G(L);
  ctx.g = g;
  ctx.owner = foreign_token_owner(L);
  ctx.released = 0;
  jit_token_rel(g, ctx.owner);
  assert(pthread_create(&th, NULL, release_jit_token_after_delay, &ctx) == 0);
  ljt_lua_dostring(L, "jit.opt.start('hotloop=3', 'hotexit=4', '-sink')\n");
  assert(pthread_join(th, NULL) == 0);
  assert(la_load32_acq(&ctx.released) == 1);
  assert(jit_token_acq(g) == 0);
  assert(jit_param_acq(G2J(g), JIT_P_hotloop) == 3);
  assert(jit_param_acq(G2J(g), JIT_P_hotexit) == 4);
  assert((jit_flags_acq(G2J(g)) & JIT_F_OPT_SINK) == 0);

  ljt_lua_dostring(L,
    "local ok = pcall(jit.opt.start, 'hotloop=5', 'not_an_option')\n"
    "assert(not ok)\n");
  assert(jit_token_acq(g) == 0);
  assert(jit_param_acq(G2J(g), JIT_P_hotloop) == 5);
}

static void clear_stopreq(TGState *tg)
{
  (void)lj_tg_flags_and_rlx(tg, (uint8_t)~(TGF_STOPREQ|TGF_STOPREQ_FRESH));
}

static void expect_token_wait_stopreq(lua_State *L, const char *code)
{
  TokenStopReqCtx ctx;
  pthread_t th;
  global_State *g = G(L);
  int status;

  memset(&ctx, 0, sizeof(ctx));
  ctx.g = g;
  ctx.tg = L2TG(L);
  ctx.owner = foreign_token_owner(L);
  clear_stopreq(ctx.tg);
  jit_token_rel(g, ctx.owner);

  assert(pthread_create(&th, NULL, publish_stopreq_while_token_waits,
			&ctx) == 0);
  status = luaL_dostring(L, code);
  assert(pthread_join(th, NULL) == 0);
  assert(la_load32_acq(&ctx.saw_native) != 0);
  assert(la_load32_acq(&ctx.signaled) >= 1u);
  assert(la_load32_acq(&ctx.released) != 0);
  assert(jit_token_acq(g) == 0);
  clear_stopreq(ctx.tg);

  if (status != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    fprintf(stderr, "unexpected token STOPREQ test error: %s\n",
	    err ? err : "(nil)");
  }
  assert(status == LUA_OK);
  lua_settop(L, 0);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g;

  g = G(L);
  assert(jit_token_acq(g) == 0);

  {
    GCtrace trace;
    SnapShot snap;
    uint8_t count;
    memset(&trace, 0, sizeof(trace));
    memset(&snap, 0, sizeof(snap));

    trace_nchild_inc_acqrel(&trace);
    assert(trace_nchild_acq(&trace) == 1);
    trace_nchild_dec_acqrel(&trace);
    assert(trace_nchild_acq(&trace) == 0);
    trace_nchild_dec_acqrel(&trace);
    assert(trace_nchild_acq(&trace) == 0);

    count = 0;
    assert(snap_count_cas_acqrel(&snap, &count, 1) != 0);
    assert(count == 0);
    assert(snap_count_acq(&snap) == 1);

    count = 0;
    assert(snap_count_cas_acqrel(&snap, &count, 2) == 0);
    assert(count == 1);
    assert(snap_count_acq(&snap) == 1);

    snap_count_rel(&snap, SNAPCOUNT_DONE);
    assert(snap_count_acq(&snap) == SNAPCOUNT_DONE);
  }

#if LJ_TARGET_X64 && !LJ_ABI_WIN
  {
    TGState secondary;
    TGState *saved_tg = lj_thr_get_tg();
    assert(g->main_tg != NULL);
    lj_tg_init_thread(g, &secondary, NULL, 0);
    secondary.tid = g->main_tg->tid == 0x7ffffffeu ? 0x7ffffffdu : 0x7ffffffeu;
    secondary.alloc.owner_tid = secondary.tid;
    lj_thr_set_tg(&secondary);
    assert(G2TG(g) == &secondary);
    assert(lj_jit_token_try(g->jitp) != 0);
    assert(jit_token_acq(g) == secondary.tid);
    assert(lj_jit_token_held(g->jitp) != 0);
    lj_jit_token_release(g->jitp);
    assert(jit_token_acq(g) == 0);
    lj_thr_set_tg(saved_tg);
    lj_tg_fini_thread(g, &secondary);
  }
#endif

  ljt_lua_dostring(L,
    "local util = require'jit.util'\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function tracecount()\n"
    "  local n = 0\n"
    "  for i = 1, 32 do if util.traceinfo(i) then n = n + 1 end end\n"
    "  return n\n"
    "end\n"
    "jit.off(tracecount, true)\n"
    "local function f(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i end\n"
    "  return s\n"
    "end\n"
    "for _ = 1, 20 do assert(f(80) == 3240) end\n"
    "assert(tracecount() > 0, 'expected token-owned recording')\n");
  assert(jit_token_acq(g) == 0);

  ljt_lua_dostring(L,
    "local util = require'jit.util'\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "function lj_m6_busy_tracecount()\n"
    "  local n = 0\n"
    "  for i = 1, 32 do if util.traceinfo(i) then n = n + 1 end end\n"
    "  return n\n"
    "end\n"
    "jit.off(lj_m6_busy_tracecount, true)\n"
    "function lj_m6_busy_f(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i end\n"
    "  return s\n"
    "end\n");
  jit_token_rel(g, 0x7fffffffu);
  ljt_lua_dostring(L,
    "for _ = 1, 40 do assert(lj_m6_busy_f(80) == 3240) end\n"
    "assert(lj_m6_busy_tracecount() == 0, 'busy recorder token must skip tracing')\n");
  assert(jit_token_acq(g) == 0x7fffffffu);

  jit_token_rel(g, 0);
  ljt_lua_dostring(L,
    "local util = require'jit.util'\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function tracecount()\n"
    "  local n = 0\n"
    "  for i = 1, 32 do if util.traceinfo(i) then n = n + 1 end end\n"
    "  return n\n"
    "end\n"
    "jit.off(tracecount, true)\n"
    "local function f(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i end\n"
    "  return s\n"
    "end\n"
    "for _ = 1, 20 do assert(f(80) == 3240) end\n"
    "assert(tracecount() > 0, 'recording should resume after token release')\n");
  assert(jit_token_acq(g) == 0);

  expect_flush_waits_for_token(L, "jit.flush()\n");
  expect_flush_waits_for_token(L, "jit.flush(1)\n");
  expect_opt_start_waits_for_token(L);
  expect_token_wait_stopreq(L,
    "local ok, err = pcall(jit.flush)\n"
    "assert(not ok and tostring(err):find('VM shutdown', 1, true))\n");
  make_token_flush_trace(L);
  expect_token_wait_stopreq(L,
    "local ok, err = pcall(jit.flush, 1)\n"
    "assert(not ok and tostring(err):find('VM shutdown', 1, true))\n");
  expect_token_wait_stopreq(L,
    "local ok, err = pcall(jit.opt.start, 'hotloop=9')\n"
    "assert(not ok and tostring(err):find('VM shutdown', 1, true))\n");

  lua_close(L);
  printf("t-jit-token OK: recorder token accepts secondary TGs and owns JIT controls\n");
  return 0;
}
