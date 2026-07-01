/*
** FFI cdef parser-token native STOPREQ guard.
*/

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_ctype.h"
#include "lj_tg.h"

typedef struct ParseReleaseCtx {
  CTState *cts;
  TGState *tg;
  uint32_t release_seq;
  int set_stopreq;
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

static void set_stopreq(TGState *tg)
{
  (void)lj_tg_flags_or_rlx(tg, TGF_STOPREQ);
}

static void clear_stopreq(TGState *tg)
{
  uint8_t flags = lj_tg_flags_acq(tg);
  lj_tg_flags_store_rlx(tg, (uint8_t)(flags & ~(TGF_STOPREQ|TGF_STOPREQ_FRESH)));
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
  if (ctx->set_stopreq)
    set_stopreq(ctx->tg);
  ctype_parse_token_rel(ctx->cts, ctx->release_seq);
  (void)ctype_parse_token_wake(ctx->cts, 1);
  return NULL;
}

static uint32_t hold_parse_token(CTState *cts)
{
  uint32_t seq = ctype_parse_token_acq(cts);
  assert((seq & 1u) == 0);
  ctype_parse_token_rel(cts, seq + 1u);
  return seq + 2u;
}

static int run_lua(lua_State *L, const char *chunk)
{
  int rc = luaL_dostring(L, chunk);
  if (rc != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    fprintf(stderr, "Lua error: %s\n", err ? err : "(nil)");
  }
  return rc;
}

static void run_ok(lua_State *L, const char *chunk)
{
  int rc = run_lua(L, chunk);
  assert(rc == LUA_OK);
}

static void run_cdef_wait(lua_State *L, CTState *cts, TGState *tg,
			  const char *chunk, int set_fresh_stopreq,
			  int expect_stopreq)
{
  ParseReleaseCtx ctx;
  pthread_t thread;
  int rc;

  ctx.cts = cts;
  ctx.tg = tg;
  ctx.release_seq = hold_parse_token(cts);
  ctx.set_stopreq = set_fresh_stopreq;
  ctx.saw_native = 0;

  assert(pthread_create(&thread, NULL, release_parse_token, &ctx) == 0);
  rc = luaL_dostring(L, chunk);
  assert(pthread_join(thread, NULL) == 0);
  assert(ctx.saw_native);
  assert((ctype_parse_token_acq(cts) & 1u) == 0);

  if (expect_stopreq) {
    const char *err;
    assert(rc != LUA_OK);
    err = lua_tostring(L, -1);
    assert(err != NULL);
    assert(strstr(err, "thread interrupted: VM shutdown") != NULL);
    lua_settop(L, 0);
  } else if (rc != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    fprintf(stderr, "unexpected Lua error: %s\n", err ? err : "(nil)");
    assert(rc == LUA_OK);
  }
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  CTState *cts;
  TGState *tg;

  assert(L != NULL);
  luaL_openlibs(L);
  run_ok(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef struct { int x; } lj_m7_cdef_stopreq_base_t;')\n"
    "assert(ffi.sizeof('lj_m7_cdef_stopreq_base_t') == 4)\n");

  g = G(L);
  cts = ctype_ctsG(g);
  tg = G2TG(g);
  assert(cts != NULL);
  assert(tg != NULL);

  set_stopreq(tg);
  run_cdef_wait(L, cts, tg,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef struct { int x; } lj_m7_cdef_stopreq_sticky_t;')\n"
    "assert(ffi.sizeof('lj_m7_cdef_stopreq_sticky_t') == 4)\n",
    0, 0);
  clear_stopreq(tg);
  assert((lj_tg_flags_acq(tg) & TGF_STOPREQ) == 0);

  run_cdef_wait(L, cts, tg,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef struct { int x; } lj_m7_cdef_stopreq_fresh_t;')\n",
    1, 1);
  clear_stopreq(tg);
  assert((lj_tg_flags_acq(tg) & TGF_STOPREQ) == 0);

  run_ok(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef struct { int x; } lj_m7_cdef_stopreq_recovery_t;')\n"
    "assert(ffi.sizeof('lj_m7_cdef_stopreq_recovery_t') == 4)\n");

  lua_close(L);
  printf("t-ffi-cdef-token-stopreq OK: parser-token STOPREQ behavior verified\n");
  return 0;
}
