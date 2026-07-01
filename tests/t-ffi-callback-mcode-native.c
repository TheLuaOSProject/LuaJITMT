/*
** Focused guard for FFI callback mcode native STOPREQ handling.
*/

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_ctype.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_tg.h"

enum {
  PAUSE_MMAP = 1,
  PAUSE_MPROTECT = 2
};

typedef struct MCodeStopReqCtx {
  global_State *g;
  TGState *tg;
  uint32_t saw_native;
  uint32_t published;
  int err;
} MCodeStopReqCtx;

static TGState *pause_tg;
static uint32_t pause_kind;
static uint32_t pause_seen;
static uint32_t pause_release;

static void sleep_ns(long ns)
{
  struct timespec ts;
  ts.tv_sec = ns / 1000000000L;
  ts.tv_nsec = ns % 1000000000L;
  while (nanosleep(&ts, &ts) != 0) {
  }
}

static void maybe_pause_native(uint32_t kind)
{
  uint32_t expect = kind;
  if (pause_tg == NULL || !lj_tg_in_native_acq(pause_tg))
    return;
  if (la_cas32(&pause_kind, &expect, 0, LA_ACQ_REL, LA_ACQ)) {
    la_store32_rel(&pause_seen, kind);
    while (la_load32_acq(&pause_release) == 0)
      sleep_ns(100000L);
    la_store32_rel(&pause_seen, 0);
  }
}

extern void *__real_mmap(void *addr, size_t length, int prot, int flags,
			 int fd, off_t offset);

void *__wrap_mmap(void *addr, size_t length, int prot, int flags,
		  int fd, off_t offset)
{
  maybe_pause_native(PAUSE_MMAP);
  return __real_mmap(addr, length, prot, flags, fd, offset);
}

extern void *__real_mmap64(void *addr, size_t length, int prot, int flags,
			   int fd, off_t offset);

void *__wrap_mmap64(void *addr, size_t length, int prot, int flags,
		    int fd, off_t offset)
{
  maybe_pause_native(PAUSE_MMAP);
  return __real_mmap64(addr, length, prot, flags, fd, offset);
}

extern int __real_mprotect(void *addr, size_t len, int prot);

int __wrap_mprotect(void *addr, size_t len, int prot)
{
  maybe_pause_native(PAUSE_MPROTECT);
  return __real_mprotect(addr, len, prot);
}

static void *publish_stopreq_while_paused(void *arg)
{
  MCodeStopReqCtx *ctx = (MCodeStopReqCtx *)arg;
  int i;
  for (i = 0; i < 1000; i++) {
    if (la_load32_acq(&pause_seen) != 0 && lj_tg_in_native_acq(ctx->tg)) {
      la_store32_rel(&ctx->saw_native, 1);
      break;
    }
    sleep_ns(100000L);
  }
  if (la_load32_acq(&ctx->saw_native) == 0) {
    ctx->err = 1;
    la_store32_rel(&pause_release, 1);
    return NULL;
  }
  if (lj_safepoint_handshake(ctx->g, LJ_GC2_HS_STOPREQ) == 0)
    ctx->err = 2;
  la_store32_rel(&ctx->published, 1);
  la_store32_rel(&pause_release, 1);
  return NULL;
}

static lua_State *new_open_state(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  return L;
}

static int require_ffi(lua_State *L)
{
  return luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "assert(type(ffi.os) == 'string')\n");
}

static void clear_stopreq(TGState *tg)
{
  (void)lj_tg_flags_and_rlx(tg, (uint8_t)~(TGF_STOPREQ|TGF_STOPREQ_FRESH));
}

static void test_sticky_stopreq_ok(void)
{
  lua_State *L = new_open_state();
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  CTState *cts;
  assert(tg != NULL);

  (void)lj_tg_flags_or_rlx(tg, TGF_STOPREQ);
  assert(require_ffi(L) == LUA_OK);
  assert(lj_tg_flags_test_acq(tg, TGF_STOPREQ));
  cts = ctype_ctsG(g);
  assert(cts != NULL);
  assert(ctype_cb_mcode_acq(cts) != NULL);
  clear_stopreq(tg);
  lua_close(L);
}

static void test_fresh_stopreq_interrupts(uint32_t kind)
{
  lua_State *L = new_open_state();
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  MCodeStopReqCtx ctx;
  pthread_t thread;
  int err, rc;

  assert(tg != NULL);
  memset(&ctx, 0, sizeof(ctx));
  ctx.g = g;
  ctx.tg = tg;
  pause_tg = tg;
  la_store32_rel(&pause_kind, kind);
  la_store32_rel(&pause_seen, 0);
  la_store32_rel(&pause_release, 0);

  err = pthread_create(&thread, NULL, publish_stopreq_while_paused, &ctx);
  assert(err == 0);
  rc = require_ffi(L);
  err = pthread_join(thread, NULL);
  assert(err == 0);
  assert(ctx.err == 0);
  assert(la_load32_acq(&ctx.saw_native) == 1);
  assert(la_load32_acq(&ctx.published) == 1);
  assert(rc != LUA_OK);
  assert(lua_tostring(L, -1) != NULL);
  assert(strstr(lua_tostring(L, -1), "thread interrupted: VM shutdown") != NULL);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_flags_test_acq(tg, TGF_STOPREQ));
  if (ctype_ctsG(g) != NULL)
    assert(ctype_cb_mcode_acq(ctype_ctsG(g)) == NULL);
  clear_stopreq(tg);
  pause_tg = NULL;
  lua_close(L);
}

int main(void)
{
  test_sticky_stopreq_ok();
  test_fresh_stopreq_interrupts(PAUSE_MMAP);
  test_fresh_stopreq_interrupts(PAUSE_MPROTECT);
  printf("t-ffi-callback-mcode-native OK: callback mcode native STOPREQ verified\n");
  return 0;
}
