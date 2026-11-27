/* Keep a raw cursor and an ordinary registry reader across TG unlink. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lib/lua_fixture_helpers.h"

static TGState *target;
static LJTGRegistryKey target_key;
static void *target_map;
static pthread_t smr_thread;
static uint32_t cursor_ready, cursor_release, smr_ready, smr_release;

static void wait_flag(uint32_t *flag)
{
  while (la_load32_acq(flag) == 0)
    lj_thr_sched_yield();
}

static TGState *target_on_list(global_State *g)
{
  TGState *tg;
  for (tg = gc2_tg_list_acq(g); tg; tg = lj_tg_next_acq(tg))
    if (tg == target)
      return tg;
  return NULL;
}

static void assert_cursor(TGState *tg, TGState *next)
{
  assert(lj_tg_flags_test_acq(tg, TGF_DEAD));
  assert(lj_tg_next_acq(tg) == next);
}

static int hold_cursor(lua_State *L)
{
  TGState *tg = target_on_list(G(L));
  TGState *next;
  assert(tg);
  next = lj_tg_next_acq(tg);
  la_store32_rel(&cursor_ready, 1);
  wait_flag(&cursor_release);
  assert_cursor(tg, next);
  return 0;
}

static void *hold_smr(void *arg)
{
  global_State *g = (global_State *)arg;
  TGState *tg, *next;
  while (!lj_gc2_smr_read_try(g))
    lj_thr_sched_yield();
  tg = target_on_list(g);
  assert(tg);
  next = lj_tg_next_acq(tg);
  la_store32_rel(&smr_ready, 1);
  wait_flag(&smr_release);
  assert_cursor(tg, next);
  lj_gc2_smr_read_leave(g);
  return NULL;
}

static int make_target(lua_State *L)
{
  global_State *g = G(L);
  target = lj_mem_newt(L, sizeof(TGState), TGState);
  assert(lj_tg_init_thread(g, target, NULL, 1));
  lj_tg_tid_rel(target, lj_thr_newid());
  lj_arena_alloc_owner_rel(&target->alloc, lj_tg_tid_acq(target));
  lj_tg_flags_or_rlx(target, TGF_LUA_ALLOC);
  lj_tg_attach(g, target);
  target_key = target->registry_key;
  lj_tg_detach(g, target);
  target_map = lj_arena_of(target);
  return 0;
}

static int begin_unlink(lua_State *L)
{
  global_State *g = G(L);
  wait_flag(&cursor_ready);
  assert(pthread_create(&smr_thread, NULL, hold_smr, g) == 0);
  wait_flag(&smr_ready);
  lj_tg_flags_or_rlx(target, TGF_DEFER_FREE);
  while (target_on_list(g))
    (void)lj_tg_reclaim_dead(g);
  return 0;
}

static int check_mapped(lua_State *L)
{
  global_State *g = G(L);
  LJTGRegistryBodySnap body;
  unsigned char resident;
  (void)lj_tg_reclaim_dead(g);
  body = lj_tgregistry_slot_body_snapshot(target_key.slot);
  assert(lj_tgregistry_body_snap_is(&body, target, target_key.incarnation));
  assert(mincore(target_map, (size_t)sysconf(_SC_PAGESIZE), &resident) == 0);
  return 0;
}

static int release_cursor(lua_State *L)
{
  UNUSED(L);
  la_store32_rel(&cursor_release, 1);
  return 0;
}

static int await_withdrawn(lua_State *L)
{
  global_State *g = G(L);
  LJTGSlotSnap slot;
  unsigned char resident;
  la_store32_rel(&smr_release, 1);
  assert(pthread_join(smr_thread, NULL) == 0);
  do {
    (void)lj_tg_reclaim_dead(g);
    assert(lj_tgregistry_key_snapshot(&target_key, &slot) == LJ_TGSLOT_OK);
    lj_thr_sched_yield();
  } while (slot.state != LJ_TGSLOT_EMPTY);
  errno = 0;
  assert(mincore(target_map, (size_t)sysconf(_SC_PAGESIZE), &resident) == -1 &&
    errno == ENOMEM);
  return 0;
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  alarm(30);
  lua_register(L, "make_target", make_target);
  lua_register(L, "hold_cursor", hold_cursor);
  lua_register(L, "begin_unlink", begin_unlink);
  lua_register(L, "check_mapped", check_mapped);
  lua_register(L, "release_cursor", release_cursor);
  lua_register(L, "await_withdrawn", await_withdrawn);
  ljt_lua_assert_ok(L, luaL_loadstring(L,
    "if jit then jit.off() end\n"
    "collectgarbage('stop')\n"
    "local th = require('threading')\n"
    "th.gcworkers(tonumber(os.getenv('POOL')) or 0)\n"
    "make_target()\n"
    "local parked, unpark = th.channel(1), th.channel(1)\n"
    "local sentinel = assert(th.spawn(function()\n"
    "  hold_cursor()\n"
    "  assert(parked:send(true, 10))\n"
    "  assert(unpark:recv(10))\n"
    "end))\n"
    "begin_unlink()\n"
    "check_mapped()\n"
    "local independent = {17, 19}\n"
    "assert(independent[2] == 19)\n"
    "release_cursor()\n"
    "assert(parked:recv(10))\n"
    "check_mapped()\n"
    "await_withdrawn()\n"
    "assert(sentinel:running())\n"
    "assert(unpark:send(true, 10))\n"
    "assert(sentinel:join(10))\n"
    "th.gcworkers(0)\n"), "load live TG reap test");
  ljt_lua_pcall(L, 0, 0, "live TG reap test");
  lua_close(L);
  puts("detached TG unlinked and unmapped with a live sentinel");
  return 0;
}
