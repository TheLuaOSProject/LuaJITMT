/* Keep a raw cursor and an ordinary registry reader across TG unlink. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
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
static uintptr_t target_mapsize;
static uint32_t target_unmapped;
static pthread_t smr_thread, control_thread;
static uint32_t cursor_ready, cursor_release, smr_ready, smr_release;
static uint32_t control_ready, control_release;

int __real_munmap(void *p, size_t size);

int __wrap_munmap(void *p, size_t size)
{
  int result = __real_munmap(p, size);
  if (result == 0 && p == la_loadptr_acq((void *const *)&target_map) &&
      size == la_loaduptr_acq(&target_mapsize))
    la_store32_rel(&target_unmapped, 1);
  return result;
}

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

static void *hold_control(void *arg)
{
  global_State *g = (global_State *)arg;
  uint32_t expect = 0;
  TGState *tg, *next;
  assert(gc2_worker_control_cas(g, &expect, 1));
  tg = target_on_list(g);
  assert(tg);
  next = lj_tg_next_acq(tg);
  la_store32_rel(&control_ready, 1);
  wait_flag(&control_release);
  assert_cursor(tg, next);
  gc2_worker_control_rel(g, 0);
  gc2_worker_control_futex_wake(g, 1);
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
  la_store32_rlx(&target_unmapped, 0);
  la_storeuptr_rel(&target_mapsize, lj_arena_huge_mapsize(sizeof(*target)));
  la_storeptr_rel(&target_map, lj_arena_of(target));
  return 0;
}

static int begin_unlink(lua_State *L)
{
  global_State *g = G(L);
  wait_flag(&cursor_ready);
  assert(pthread_create(&smr_thread, NULL, hold_smr, g) == 0);
  wait_flag(&smr_ready);
  assert(pthread_create(&control_thread, NULL, hold_control, g) == 0);
  wait_flag(&control_ready);
  lj_tg_flags_or_rlx(target, TGF_DEFER_FREE);
  while (target_on_list(g))
    (void)lj_tg_reclaim_empty_dead(g);
  return 0;
}

static int check_mapped(lua_State *L)
{
  global_State *g = G(L);
  LJTGRegistryBodySnap body;
  (void)lj_tg_reclaim_empty_dead(g);
  body = lj_tgregistry_slot_body_snapshot(target_key.slot);
  assert(lj_tgregistry_body_snap_is(&body, target, target_key.incarnation));
  assert(!la_load32_acq(&target_unmapped));
  return 0;
}

static int release_cursor(lua_State *L)
{
  UNUSED(L);
  la_store32_rel(&cursor_release, 1);
  return 0;
}

static int release_smr(lua_State *L)
{
  UNUSED(L);
  la_store32_rel(&smr_release, 1);
  assert(pthread_join(smr_thread, NULL) == 0);
  return 0;
}

static int release_control(lua_State *L)
{
  UNUSED(L);
  la_store32_rel(&control_release, 1);
  assert(pthread_join(control_thread, NULL) == 0);
  return 0;
}

static int nested_collect(lua_State *L)
{
  global_State *g = G(L);
  TGState *held = target_on_list(g);
  TGState *next;
  assert(held);
  next = lj_tg_next_acq(held);
  lj_tg_flags_or_rlx(held, TGF_DEFER_FREE);
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(target_on_list(g) == held);
  lua_getglobal(L, "collectgarbage");
  lua_call(L, 0, 0);
  assert(target_on_list(g) == held);
  assert_cursor(held, next);
  return 0;
}

static int check_unlinked(lua_State *L)
{
  assert(target_on_list(G(L)) == NULL);
  return 0;
}

static int await_withdrawn(lua_State *L)
{
  global_State *g = G(L);
  LJTGSlotSnap slot;
  do {
    (void)lj_tg_reclaim_empty_dead(g);
    assert(lj_tgregistry_key_snapshot(&target_key, &slot) == LJ_TGSLOT_OK);
    lj_thr_sched_yield();
  } while (slot.state != LJ_TGSLOT_EMPTY);
  assert(la_load32_acq(&target_unmapped));
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
  lua_register(L, "release_smr", release_smr);
  lua_register(L, "release_control", release_control);
  lua_register(L, "nested_collect", nested_collect);
  lua_register(L, "check_unlinked", check_unlinked);
  lua_register(L, "await_withdrawn", await_withdrawn);
  ljt_lua_assert_ok(L, luaL_loadstring(L,
    "if jit and os.getenv('JIT') ~= 'on' then jit.off() end\n"
    "collectgarbage('stop')\n"
    "local th = require('threading')\n"
    "th.gcworkers(tonumber(os.getenv('POOL')) or 0)\n"
    "make_target()\n"
    "local parked, unpark = th.channel(1), th.channel(1)\n"
    "local spin, done = os.getenv('SPIN') == '1', false\n"
    "local sentinel = assert(th.spawn(function()\n"
    "  hold_cursor()\n"
    "  assert(parked:send(true, 10))\n"
    "  if spin then\n"
    "    while not done do end\n"
    "  else\n"
    "    assert(unpark:recv(10))\n"
    "  end\n"
    "end))\n"
    "begin_unlink()\n"
    "check_mapped()\n"
    "local independent = {17, 19}\n"
    "assert(independent[2] == 19)\n"
    "release_cursor()\n"
    "assert(parked:recv(10))\n"
    "check_mapped()\n"
    "release_smr()\n"
    "if spin then for _ = 1, 3 do collectgarbage('collect') end end\n"
    "check_mapped()\n"
    "release_control()\n"
    "await_withdrawn()\n"
    "if not spin then\n"
    "  make_target()\n"
    "  nested_collect()\n"
    "  for _ = 1, 3 do collectgarbage('collect') end\n"
    "  check_unlinked()\n"
    "  await_withdrawn()\n"
    "end\n"
    "assert(sentinel:running())\n"
    "if spin then done = true else assert(unpark:send(true, 10)) end\n"
    "assert(sentinel:join(10))\n"
    "th.gcworkers(0)\n"), "load live TG reap test");
  ljt_lua_pcall(L, 0, 0, "live TG reap test");
  lua_close(L);
  puts("detached TG unlinked and unmapped with a live sentinel");
  return 0;
}
