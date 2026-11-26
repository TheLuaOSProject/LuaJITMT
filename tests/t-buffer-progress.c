/* Public buffer operations must finish while a writer or metadata collector
** is suspended, before storage admission and with real storage leases held. */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "lj_obj.h"
#include "lj_buf.h"
#include "lj_gc2.h"
#include "lj_thr.h"
#include "lib/lua_fixture_helpers.h"

#if !defined(LJ_BUF_TEST_HELPERS) || !defined(LJ_GC2_TEST_HELPERS)
#error "t-buffer-progress requires LJ_BUF_TEST_HELPERS and LJ_GC2_TEST_HELPERS"
#endif
#if !LJ_HASBUFFER || !LJ_HASFFI
#error "t-buffer-progress requires the buffer and FFI libraries"
#endif

enum { WRITER_IDLE, WRITER_ARMED, WRITER_HELD, WRITER_RELEASED };
static lua_State *main_L;
static void *writer_buf;
static uint32_t writer_stage, writer_state, writer_hits;

static void pause_writer(lua_State *L, LJBuffer *buf, uint32_t stage)
{
  uint32_t expected = WRITER_ARMED;
  if (L == main_L || la_load32_acq(&writer_state) != WRITER_ARMED ||
      stage != la_load32_acq(&writer_stage) ||
      buf != (LJBuffer *)la_loadptr_acq(&writer_buf) ||
      !la_cas32(&writer_state, &expected, WRITER_HELD, LA_ACQ_REL, LA_ACQ))
    return;
  (void)la_add32_rlx(&writer_hits, 1);
  /* Deliberately provide no native region or safepoint certificate. Depending
  ** on the stage, storage admission is pending or its real leases stay held.
  ** Automatic GC is stopped in this test. */
  while (la_load32_acq(&writer_state) == WRITER_HELD)
    sched_yield();
}

static int arm_writer(lua_State *L)
{
  uint32_t stage = (uint32_t)luaL_checkinteger(L, 2);
  assert(L == main_L && tvisbuf(L->base));
  assert(la_load32_acq(&writer_state) == WRITER_IDLE);
  assert(stage == LJ_BUF_TEST_TAIL_CLAIMED ||
         stage == LJ_BUF_TEST_BEFORE_PUBLISH ||
         stage == LJ_BUF_TEST_STORAGE_SAMPLED);
  la_storeptr_rel(&writer_buf, bufV(L->base));
  la_store32_rel(&writer_stage, stage);
  la_store32_rel(&writer_hits, 0);
  la_store32_rel(&writer_state, WRITER_ARMED);
  return 0;
}

static int wait_writer(lua_State *L)
{
  assert(L == main_L);
  while (la_load32_acq(&writer_state) == WRITER_ARMED)
    (void)lj_thr_retry_yield(L);
  assert(la_load32_acq(&writer_state) == WRITER_HELD);
  return 0;
}

static int release_writer(lua_State *L)
{
  assert(L == main_L);
  assert(la_load32_acq(&writer_state) == WRITER_HELD);
  la_store32_rel(&writer_state, WRITER_RELEASED);
  return 0;
}

static int writer_finished(lua_State *L)
{
  assert(L == main_L);
  assert(la_load32_acq(&writer_state) == WRITER_RELEASED);
  assert(la_load32_acq(&writer_hits) == 1);
  la_storeptr_rel(&writer_buf, NULL);
  la_store32_rel(&writer_state, WRITER_IDLE);
  return 0;
}

static void check_suspended_writers(lua_State *L)
{
  lua_gc(L, LUA_GCSTOP, 0);
  lj_buf_test_set_hook(pause_writer);
  ljt_lua_loadstring(L,
    "local tail, publish, sampled = ...\n"
    "local th, buf = require('threading'), require('string.buffer')\n"
    "local havejit, jit = pcall(require, 'jit')\n"
    "if havejit then jit.off() end\n"
    "local function append(b) b:put('writer|'); return b:tostring() end\n"
    "local cases = {{tail, false, 'consume'},\n"
    "               {publish, true, 'reset'}, {publish, true, 'free'},\n"
    "               {sampled, false, 'consume'}, {sampled, false, 'free'}}\n"
    "for _, case in ipairs(cases) do\n"
    "  local b = buf.new(64)\n"
    "  if case[2] then b:set('prefix|') else b:put('prefix|') end\n"
    "  buffer_progress_arm(b, case[1])\n"
    "  local writer = th.spawn(append, b)\n"
    "  buffer_progress_wait()\n"
    "  assert(b:tostring() == 'prefix|')\n"
    "  b:put('peer|')\n"
    "  assert(b:tostring() == 'prefix|peer|')\n"
    "  local expected\n"
    "  if case[3] == 'consume' then\n"
    "    assert(b:get(7) == 'prefix|'); expected = 'peer|'\n"
    "  elseif case[3] == 'reset' then\n"
    "    b:reset():put('reset|'); expected = 'reset|'\n"
    "  else\n"
    "    b:free():put('free|'); expected = 'free|'\n"
    "  end\n"
    "  assert(b:tostring() == expected)\n"
    "  buffer_progress_release()\n"
    "  local ok, result = writer:join(10); assert(ok, result)\n"
    "  buffer_progress_finished()\n"
    "  assert(result == expected .. 'writer|')\n"
    "  assert(b:get() == expected .. 'writer|' and #b == 0)\n"
    "end\n");
  lua_pushinteger(L, LJ_BUF_TEST_TAIL_CLAIMED);
  lua_pushinteger(L, LJ_BUF_TEST_BEFORE_PUBLISH);
  lua_pushinteger(L, LJ_BUF_TEST_STORAGE_SAMPLED);
  ljt_lua_pcall(L, 3, 0, "buffer writer suspension");
  lj_buf_test_set_hook(NULL);
  assert(la_load32_acq(&writer_state) == WRITER_IDLE);
  assert(!lj_gc2_activation_reclaim_veto(G(L)));
}

typedef struct Reclaim {
  global_State *g;
  uint32_t done;
} Reclaim;

static void *reclaim_metadata(void *arg)
{
  Reclaim *ctx = (Reclaim *)arg;
  (void)lj_gc2_reclaim_retired(ctx->g, lj_gc2_retire_epoch(ctx->g) + 1);
  la_store32_rel(&ctx->done, 1);
  return NULL;
}

static void check_suspended_metadata(lua_State *L)
{
  global_State *g = G(L);
  pthread_t collector;
  Reclaim ctx = { g, 0 };
  assert(lua_gettop(L) == 0);
  ljt_lua_dostring(L,
    "local ffi, buf = require('ffi'), require('string.buffer')\n"
    "local n = 65537\n"
    "local bytes = string.rep('S', n-4) .. '\\0end'\n"
    "local array = ffi.new('uint8_t[?]', n)\n"
    "ffi.copy(array, bytes, n)\n"
    "local read_s, read_c = buf.new():set(bytes), buf.new():set(array, n)\n"
    "local grow_s, grow_c = buf.new():set(bytes), buf.new():set(array, n)\n"
    "local owned = buf.new():put(bytes)\n"
    "local reserved, fresh, consumed\n"
    "return function()\n"
    "  assert(#read_s == n and #read_c == n)\n"
    "  assert(read_s:tostring() == bytes and read_c:tostring() == bytes)\n"
    "  local p, len = read_s:ref()\n"
    "  assert(len == n and ffi.string(p, len) == bytes)\n"
    "  p, len = read_c:ref()\n"
    "  assert(len == n and ffi.string(p, len) == bytes)\n"
    "  grow_s:put('tail'); grow_c:put('tail')\n"
    "  assert(grow_s:tostring() == bytes .. 'tail')\n"
    "  assert(grow_c:tostring() == bytes .. 'tail')\n"
    "  owned:put(bytes)\n"
    "  assert(owned:tostring() == bytes .. bytes)\n"
    "  owned:reset():put('reset'); assert(owned:tostring() == 'reset')\n"
    "  owned:free():put(bytes); assert(owned:tostring() == bytes)\n"
    "  fresh = buf.new(n):put(bytes)\n"
    "  assert(fresh:tostring() == bytes)\n"
    "  reserved = buf.new()\n"
    "  p, len = reserved:reserve(n); assert(len >= n)\n"
    "  ffi.copy(p, bytes, n); reserved:commit(n)\n"
    "  assert(reserved:tostring() == bytes)\n"
    "  consumed = buf.new():set(array, n)\n"
    "  assert(consumed:get(3) == bytes:sub(1,3))\n"
    "  consumed:skip(2)\n"
    "  assert(consumed:tostring() == bytes:sub(6))\n"
    "end, function()\n"
    "  assert(read_s:tostring() == bytes and read_c:tostring() == bytes)\n"
    "  assert(grow_s:tostring() == bytes .. 'tail')\n"
    "  assert(grow_c:tostring() == bytes .. 'tail')\n"
    "  assert(owned:tostring() == bytes and fresh:tostring() == bytes)\n"
    "  assert(reserved:get() == bytes and #reserved == 0)\n"
    "  assert(consumed:get() == bytes:sub(6) and #consumed == 0)\n"
    "end\n");
  assert(lua_gettop(L) == 2);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(!lj_gc2_activation_reclaim_veto(g));

  lj_gc2_test_idle_reclaim_pause_after_jit_quiescence();
  assert(pthread_create(&collector, NULL, reclaim_metadata, &ctx) == 0);
  while (!lj_gc2_test_idle_reclaim_paused() && !la_load32_acq(&ctx.done))
    sched_yield();
  assert(lj_gc2_test_idle_reclaim_paused());
  assert(gc2_smr_reclaiming_acq(g) == LJ_GC2_SMR_META_EXCLUSIVE);
  lua_pushvalue(L, 1);
  ljt_lua_pcall(L, 0, 0, "buffer operations during metadata retirement");
  assert(lj_gc2_test_idle_reclaim_paused());
  assert(!la_load32_acq(&ctx.done));
  assert(gc2_smr_reclaiming_acq(g) == LJ_GC2_SMR_META_EXCLUSIVE);
  assert(!lj_gc2_activation_reclaim_veto(g));
  lj_gc2_test_idle_reclaim_release();
  assert(pthread_join(collector, NULL) == 0);
  assert(la_load32_acq(&ctx.done));
  assert(gc2_smr_reclaiming_acq(g) == LJ_GC2_SMR_OPEN);
  assert(!lj_gc2_activation_reclaim_veto(g));

  /* The publications made while the tactical markers were excluded must be
  ** found by ordinary userdata traversal in a subsequent real collection. */
  lua_gc(L, LUA_GCRESTART, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_pushvalue(L, 2);
  ljt_lua_pcall(L, 0, 0, "buffer contents after metadata retirement and GC");
  assert(!lj_gc2_activation_reclaim_veto(g));
  lua_settop(L, 0);
}

int main(void)
{
  lua_State *L = main_L = ljt_lua_newstate_openlibs();
  alarm(60);
  lua_register(L, "buffer_progress_arm", arm_writer);
  lua_register(L, "buffer_progress_wait", wait_writer);
  lua_register(L, "buffer_progress_release", release_writer);
  lua_register(L, "buffer_progress_finished", writer_finished);
  check_suspended_writers(L);
  check_suspended_metadata(L);
  lua_close(L);
  alarm(0);
  puts("t-buffer-progress OK");
  return 0;
}
