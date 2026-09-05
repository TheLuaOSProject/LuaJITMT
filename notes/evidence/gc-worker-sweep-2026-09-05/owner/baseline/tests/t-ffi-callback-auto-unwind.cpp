/*
** TLS-less FFI callback error-unwind and scoped auto-detach regression.
**
** This must be C++: an unprotected Lua error is allowed to cross the foreign
** callback boundary and is caught by catch (...).  The callback's automatic
** TG attachment must remain live until the Lua/callback frames are unwound,
** and must be completely detached before control reaches the foreign catch.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wextra"
#endif
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_atomic.h"
#include "lj_obj.h"
#include "lj_ctype.h"
#include "lj_ccallback.h"
#include "lj_gc2.h"
#include "lj_tg.h"
#include "lj_thr.h"

#include "lib/lua_fixture_helpers.h"
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

typedef int (*AutoUnwindCallback)(int);

enum WorkerMode {
  WORKER_INSPECT_DETACH = 1,
  WORKER_CLOSE_LIFETIME = 2
};

struct WorkerCtx {
  global_State *g;
  int mode;
  int returned;
  int caught;
  int guard_destroyed;
  int catch_exited;
  TGState *after_tg;
  uint32_t live_after;
};

struct CloseReleaseCtx {
  global_State *g;
  int saw_shutdown;
  int saw_live_token;
  int saw_close_blocked;
};

class UnwindGuard {
public:
  explicit UnwindGuard(int *destroyed) : destroyed_(destroyed) {}
  ~UnwindGuard() { *destroyed_ = 1; }

private:
  int *destroyed_;
};

static lua_State *mainL;
static CTState *saved_cts;
static AutoUnwindCallback saved_cb;
static MSize saved_slot;
static lua_State *saved_owner;
static TGState *throw_tg;
static lua_State *throw_L;
static uint32_t worker_mode;
static uint32_t callback_entered;
static uint32_t callback_release;
static uint32_t callback_wait_timed_out;
static uint32_t close_done;

static void sleep_1ms(void)
{
  (void)lj_thr_sleep_ns(NULL, 1000000);
}

static void wait_for_flag(uint32_t *flag)
{
  int i;
  for (i = 0; i < 5000; i++) {
    if (la_load32_acq(flag) != 0)
      return;
    sleep_1ms();
  }
  assert(la_load32_acq(flag) != 0);
}

static int tg_is_listed(global_State *g, TGState *target)
{
  TGState *tg;
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg)) {
    if (tg == target)
      return 1;
  }
  return 0;
}

static MSize dead_tg_count(global_State *g)
{
  MSize count = 0;
  TGState *tg;
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg)) {
    if (lj_tg_flags_test_acq(tg, TGF_DEAD))
      count++;
  }
  return count;
}

extern "C" void capture_auto_unwind_cb(AutoUnwindCallback cb)
{
  CTypeID1 *cbid;
  saved_cts = ctype_cts(mainL);
  saved_cb = cb;
  saved_slot = lj_ccallback_ptr2slot(saved_cts,
                                    reinterpret_cast<void *>(cb));
  assert(saved_slot != ~0u);
  assert(saved_slot < la_load32_acq(&saved_cts->cb.sizeid));
  cbid = reinterpret_cast<CTypeID1 *>(
    la_loadptr_acq(reinterpret_cast<void *const *>(&saved_cts->cb.cbid)));
  assert(cbid != NULL);
  assert(la_load16_acq(&cbid[saved_slot]) != 0);
  {
    lua_State **owners = reinterpret_cast<lua_State **>(
      la_loadptr_acq(reinterpret_cast<void *const *>(&saved_cts->cb.owner)));
    assert(owners != NULL);
    saved_owner = reinterpret_cast<lua_State *>(
      la_loadptr_acq(reinterpret_cast<void *const *>(&owners[saved_slot])));
  }
  assert(saved_owner != NULL && saved_owner != mainL);
  assert(saved_owner->tg_hint == NULL);
}

extern "C" int auto_unwind_throw(lua_State *L)
{
  TGState *tg = lj_thr_get_tg();
  uint32_t mode = la_load32_acq(&worker_mode);
  assert(tg != NULL && tg != G(L)->main_tg);
  assert(tg->gl == G(L));
  assert(L2TG(L) == tg);
  assert(lj_tg_load_cur_L(tg) == L);
  assert(lj_tg_load_thread_L(tg) == L);
  assert(ccallback_depth_acq(&tg->cb) == 1);
  assert(ccallback_L_acq(&tg->cb) == L);
  assert(ccallback_slot_acq(&tg->cb) == saved_slot);
  assert(ccallback_auto_detach_acq(&tg->cb) == 0);
  assert(tg->cb.frame[0].auto_detach == 1);
  assert(mt_live_acq(G(L)) == 1);
  throw_tg = tg;
  throw_L = L;

  if (mode == WORKER_CLOSE_LIFETIME) {
    int i;
    la_store32_rel(&callback_entered, 1);
    for (i = 0; i < 5000 && la_load32_acq(&callback_release) == 0; i++)
      sleep_1ms();
    if (la_load32_acq(&callback_release) == 0)
      la_store32_rel(&callback_wait_timed_out, 1);
  }
  return luaL_error(L, "intentional TLS-less callback unwind");
}

static void assert_detached_callback_tg(WorkerCtx *ctx)
{
  TGState *tg = throw_tg;
  assert(tg != NULL);
  assert(lj_thr_get_tg() == NULL);
  assert(mt_live_acq(ctx->g) == 0);
  assert(mt_entering_acq(ctx->g) == 0);
  assert(gc2_n_threads_acq(ctx->g) == 1);
  assert(tg_is_listed(ctx->g, tg));
  assert(lj_tg_flags_test_acq(tg, TGF_DEAD));
  assert(lj_tg_load_cur_L(tg) == NULL);
  assert(lj_tg_load_thread_L(tg) == NULL);
  assert(lj_tg_load_thread_ud(tg) == NULL);
  assert(ccallback_depth_acq(&tg->cb) == 0);
  assert(ccallback_L_acq(&tg->cb) == NULL);
  assert(ccallback_slot_acq(&tg->cb) == 0);
  assert(ccallback_auto_detach_acq(&tg->cb) == 0);
  assert(ccallback_native_had_stopreq_acq(&tg->cb) == 0);
  assert(lj_tg_ffi_call_func_acq(tg) == NULL);
  assert(throw_L != NULL && throw_L->tg_hint == NULL);
  ctx->live_after = mt_live_acq(ctx->g);
}

static void *foreign_unwind_worker(void *arg)
{
  WorkerCtx *ctx = static_cast<WorkerCtx *>(arg);
  int smr_held = ctx->mode == WORKER_INSPECT_DETACH;
  assert(lj_thr_get_tg() == NULL);
  if (smr_held)
    lj_gc2_smr_read_enter(ctx->g);
  try {
    UnwindGuard guard(&ctx->guard_destroyed);
    (void)saved_cb(42);
    ctx->returned = 1;
  } catch (...) {
    ctx->caught = 1;
    ctx->after_tg = lj_thr_get_tg();
    assert(ctx->guard_destroyed == 1);
    assert(ctx->after_tg == NULL);
    if (ctx->mode == WORKER_INSPECT_DETACH)
      assert_detached_callback_tg(ctx);
  }
  if (smr_held)
    lj_gc2_smr_read_leave(ctx->g);
  ctx->catch_exited = 1;
  return NULL;
}

static void *release_close_callback(void *arg)
{
  CloseReleaseCtx *ctx = static_cast<CloseReleaseCtx *>(arg);
  int i;
  assert(lj_thr_get_tg() == NULL);
  for (i = 0; i < 5000; i++) {
    if (mt_shutdown_acq(ctx->g) != 0) {
      ctx->saw_shutdown = 1;
      break;
    }
    sleep_1ms();
  }
  if (ctx->saw_shutdown) {
    for (i = 0; i < 25; i++)
      sleep_1ms();
    ctx->saw_close_blocked = la_load32_acq(&close_done) == 0;
    ctx->saw_live_token = mt_live_acq(ctx->g) == 1;
  }
  /* Do not touch g after this publication: the callback unwind may release
  ** its final universe token and allow lua_close() to reclaim it immediately.
  */
  la_store32_rel(&callback_release, 1);
  return NULL;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  WorkerCtx inspect = {};
  WorkerCtx closing = {};
  CloseReleaseCtx release_ctx = {};
  pthread_t worker;
  pthread_t releaser;
  assert(L != NULL);
  luaL_openlibs(L);
  mainL = L;
  g = G(L);

  lua_pushcfunction(L, auto_unwind_throw);
  lua_setglobal(L, "lj_m7_auto_unwind_throw");
  lua_pushlightuserdata(L,
    reinterpret_cast<void *>(capture_auto_unwind_cb));
  lua_setglobal(L, "lj_m7_capture_auto_unwind_cb");
  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef int (*lj_m7_auto_unwind_cb_t)(int);\n"
    "typedef void (*lj_m7_capture_auto_unwind_cb_t)(\n"
    "  lj_m7_auto_unwind_cb_t);\n"
    "]]\n"
    "local cb = ffi.cast('lj_m7_auto_unwind_cb_t', function(_)\n"
    "  return lj_m7_auto_unwind_throw()\n"
    "end)\n"
    "local capture = ffi.cast('lj_m7_capture_auto_unwind_cb_t',\n"
    "                         lj_m7_capture_auto_unwind_cb)\n"
    "capture(cb)\n"
    "m7_auto_unwind_keep = cb\n");
  assert(saved_cb != NULL);
  assert(saved_owner != NULL && saved_owner->tg_hint == NULL);
  assert(mt_live_acq(g) == 0);
  assert(gc2_n_threads_acq(g) == 1);
  assert(gc2_n_workers_acq(g) == 0);

  la_store32_rel(&worker_mode, WORKER_INSPECT_DETACH);
  inspect.g = g;
  inspect.mode = WORKER_INSPECT_DETACH;
  assert(pthread_create(&worker, NULL, foreign_unwind_worker, &inspect) == 0);
  assert(pthread_join(worker, NULL) == 0);
  assert(inspect.returned == 0);
  assert(inspect.caught == 1);
  assert(inspect.guard_destroyed == 1);
  assert(inspect.catch_exited == 1);
  assert(inspect.after_tg == NULL);
  assert(inspect.live_after == 0);
  lj_gc2_smr_read_enter(g);
  assert(dead_tg_count(g) == 1);
  {
    int i;
    for (i = 0; i < 64 && lj_tg_ssb_refs_acq(throw_tg) != 0; i++)
      (void)lj_gc2_worker_drain(g, 256);
    assert(lj_tg_ssb_refs_acq(throw_tg) == 0);
  }
  lj_gc2_smr_read_leave(g);
  assert(gc2_smr_readers_acq(g) == 0);
  assert(lj_tg_reclaim_dead(g) == 1);
  assert(!tg_is_listed(g, throw_tg));
  assert(dead_tg_count(g) == 0);

  throw_tg = NULL;
  throw_L = NULL;
  la_store32_rel(&callback_entered, 0);
  la_store32_rel(&callback_release, 0);
  la_store32_rel(&callback_wait_timed_out, 0);
  la_store32_rel(&close_done, 0);
  la_store32_rel(&worker_mode, WORKER_CLOSE_LIFETIME);
  closing.g = g;
  closing.mode = WORKER_CLOSE_LIFETIME;
  release_ctx.g = g;

  assert(pthread_create(&worker, NULL, foreign_unwind_worker, &closing) == 0);
  wait_for_flag(&callback_entered);
  assert(mt_live_acq(g) == 1);
  assert(pthread_create(&releaser, NULL, release_close_callback,
                        &release_ctx) == 0);

  /* shutdown must block on the auto-attach lifetime token until the callback
  ** is allowed to throw and the platform unwinder discharges that token. */
  lua_close(L);
  la_store32_rel(&close_done, 1);

  assert(pthread_join(releaser, NULL) == 0);
  assert(pthread_join(worker, NULL) == 0);
  assert(release_ctx.saw_shutdown == 1);
  assert(release_ctx.saw_live_token == 1);
  assert(release_ctx.saw_close_blocked == 1);
  assert(la_load32_acq(&callback_wait_timed_out) == 0);
  assert(closing.returned == 0);
  assert(closing.caught == 1);
  assert(closing.guard_destroyed == 1);
  assert(closing.catch_exited == 1);
  assert(closing.after_tg == NULL);
  assert(la_load32_acq(&close_done) == 1);

  printf("t-ffi-callback-auto-unwind OK: C++ catch detaches callback TG, "
         "reclaims it, and releases close lifetime exactly\n");
  return 0;
}
