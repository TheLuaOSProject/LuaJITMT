/*
** Focused regression test for per-table structural ownership.
*/

#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lib/tg_stopreq_fixture_helpers.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_safepoint.h"
#include "lj_tab.h"
#include "lj_thr.h"
#include "lj_tg.h"

/* Built by the M5 harness with LJ_TAB_TEST_HELPERS enabled. */

typedef struct WorkerCtx {
  lua_State *L;
  volatile int *ready;
  volatile int *start;
  volatile int *release;
  volatile int *entered;
  int hold;
  int no_l;
  int status;
} WorkerCtx;

typedef struct ResizeCtx {
  lua_State *L;
  volatile int *ready;
  volatile int *start;
  volatile int *done;
  uint32_t asize;
  uint32_t hbits;
  int clear;
  int mutate;
  int status;
} ResizeCtx;

static GCtab *resize_hook_tab;
static volatile int *resize_hook_entered;
static volatile int *resize_hook_release;
static pthread_t array_copy_owner;

static int load_flag(volatile int *p)
{
  return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void store_flag(volatile int *p, int v)
{
  __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static void add_flag(volatile int *p, int v)
{
  (void)__atomic_add_fetch(p, v, __ATOMIC_ACQ_REL);
}

static int wait_for_flag(volatile int *p, int want, uint32_t spins)
{
  while (spins-- > 0) {
    if (load_flag(p) == want)
      return 1;
    sched_yield();
  }
  return load_flag(p) == want;
}

static int wait_for_count(uint32_t (*read_count)(void), uint32_t want,
			  uint32_t spins)
{
  while (spins-- > 0) {
    if (read_count() >= want)
      return 1;
    sched_yield();
  }
  return read_count() >= want;
}

static void assert_lua_ok(lua_State *L, int rc)
{
  if (rc != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    fprintf(stderr, "Lua error: %s\n", err ? err : "(nil)");
  }
  assert(rc == LUA_OK);
}

static void publish_manual(global_State *g, TGState *tg, uint32_t actions)
{
  uint64_t epoch = la_load64_rlx(&g->gc2.hs_epoch) + 1u;
  la_store32_rel(&g->gc2.hs_actions, actions);
  la_store32_rel(&g->gc2.hs_pending, 1);
  la_store64_rel(&g->gc2.hs_epoch, epoch);
  la_store32_rel(&tg->reqmask, actions);
  la_store32_rel(&tg->poll, 1);
}

static int tab_wait_l_lua(lua_State *L)
{
  lj_tab_wait_l(L);
  return 0;
}

static int publish_stopreq_and_tab_wait_l_lua(lua_State *L)
{
  publish_manual(G(L), L2TG(L), LJ_GC2_HS_STOPREQ);
  lj_tab_wait_l(L);
  return 0;
}

static GCtab *thread_table(lua_State *L)
{
  if (!tvistab(L->top - 1))
    return NULL;
  return tabV(L->top - 1);
}

static void *struct_owner_worker(void *arg)
{
  WorkerCtx *ctx = (WorkerCtx *)arg;
  lua_State *L = ctx->L;
  GCtab *t;
  int guard;

  if (!lj_threading_attach(L)) {
    ctx->status = 1;
    return NULL;
  }
  t = thread_table(L);
  if (!t) {
    ctx->status = 2;
    lj_threading_detach(L, 1);
    return NULL;
  }

  add_flag(ctx->ready, 1);
  while (!load_flag(ctx->start))
    sched_yield();

  guard = lj_tab_struct_enter(ctx->no_l ? NULL : L, t);
  store_flag(ctx->entered, 1);
  if (ctx->hold) {
    while (!load_flag(ctx->release))
      sched_yield();
  }
  lj_tab_struct_leave(t, guard);
  lj_threading_detach(L, 1);
  ctx->status = 0;
  return NULL;
}

static void resize_hold_hook(lua_State *L, GCtab *t, TValue *oldarray,
			     MSize oldasize)
{
  UNUSED(L);
  UNUSED(oldarray);
  UNUSED(oldasize);
  if (t != resize_hook_tab)
    return;
  store_flag(resize_hook_entered, 1);
  while (!load_flag(resize_hook_release))
    sched_yield();
}

static void hash_cutover_mutate(lua_State *L, int read_first)
{
  if (read_first) {
    lua_rawgeti(L, 1, -1);
    assert(lua_tointeger(L, -1) == 11);
    lua_pop(L, 1);
  }
  lua_pushinteger(L, 111);
  lua_rawseti(L, 1, -1);
  lua_pushnil(L);
  lua_rawseti(L, 1, -2);
  lua_pushinteger(L, 333);
  lua_rawseti(L, 1, -3);
}

static void array_copy_hold_hook(GCtab *t, MSize idx)
{
  if (t != resize_hook_tab || idx != 1 ||
      !pthread_equal(pthread_self(), array_copy_owner))
    return;
  store_flag(resize_hook_entered, 1);
  while (!load_flag(resize_hook_release))
    sched_yield();
}

static void array_migration_mutate(lua_State *L)
{
  lua_rawgeti(L, 1, 2);
  assert(lua_tointeger(L, -1) == 22);
  lua_pop(L, 1);
  lua_pushnil(L);
  lua_rawseti(L, 1, 1);
  lua_pushinteger(L, 222);
  lua_rawseti(L, 1, 2);
  lua_pushinteger(L, 444);
  lua_rawseti(L, 1, 64);
}

static void *resize_worker(void *arg)
{
  ResizeCtx *ctx = (ResizeCtx *)arg;
  lua_State *L = ctx->L;
  GCtab *t;

  if (!lj_threading_attach(L)) {
    ctx->status = 1;
    return NULL;
  }
  t = thread_table(L);
  if (!t) {
    ctx->status = 2;
    lj_threading_detach(L, 1);
    return NULL;
  }

  add_flag(ctx->ready, 1);
  while (!load_flag(ctx->start))
    sched_yield();

  if (ctx->clear)
    lj_tab_clear(L, t);
  else if (ctx->mutate == 3)
    array_migration_mutate(L);
  else if (ctx->mutate)
    hash_cutover_mutate(L, ctx->mutate == 2);
  else
    lj_tab_resize(L, t, ctx->asize, ctx->hbits);
  store_flag(ctx->done, 1);
  lj_threading_detach(L, 1);
  ctx->status = 0;
  return NULL;
}

static lua_State *new_child_with_table(lua_State *L, int table_index)
{
  lua_State *child = lua_newthread(L);
  lua_pushvalue(L, table_index);
  lua_xmove(L, child, 1);
  return child;
}

static void exercise_direct_owner(lua_State *L)
{
  lua_State *ownerL, *otherL, *sameNoLL, *sameLL;
  pthread_t owner_thr, other_thr, same_no_l_thr, same_l_thr;
  WorkerCtx owner, other, same_no_l, same_l;
  volatile int ready = 0;
  volatile int owner_start = 0;
  volatile int other_start = 0;
  volatile int same_no_l_start = 0;
  volatile int same_l_start = 0;
  volatile int owner_release = 0;
  volatile int no_release = 0;
  volatile int owner_entered = 0;
  volatile int other_entered = 0;
  volatile int same_no_l_entered = 0;
  volatile int same_l_entered = 0;

  lua_settop(L, 0);
  lua_newtable(L);  /* table A */
  lua_newtable(L);  /* table B */
  assert(lua_gettop(L) == 2);

  ownerL = new_child_with_table(L, 1);
  otherL = new_child_with_table(L, 2);
  sameNoLL = new_child_with_table(L, 1);
  sameLL = new_child_with_table(L, 1);

  owner.L = ownerL;
  owner.ready = &ready;
  owner.start = &owner_start;
  owner.release = &owner_release;
  owner.entered = &owner_entered;
  owner.hold = 1;
  owner.no_l = 0;
  owner.status = -1;

  other.L = otherL;
  other.ready = &ready;
  other.start = &other_start;
  other.release = &no_release;
  other.entered = &other_entered;
  other.hold = 0;
  other.no_l = 0;
  other.status = -1;

  same_no_l.L = sameNoLL;
  same_no_l.ready = &ready;
  same_no_l.start = &same_no_l_start;
  same_no_l.release = &no_release;
  same_no_l.entered = &same_no_l_entered;
  same_no_l.hold = 0;
  same_no_l.no_l = 1;
  same_no_l.status = -1;

  same_l.L = sameLL;
  same_l.ready = &ready;
  same_l.start = &same_l_start;
  same_l.release = &no_release;
  same_l.entered = &same_l_entered;
  same_l.hold = 0;
  same_l.no_l = 0;
  same_l.status = -1;

  lj_tab_test_reset_struct_owner_l_waits();
  lj_tab_test_reset_struct_owner_no_l_waits();

  assert(pthread_create(&owner_thr, NULL, struct_owner_worker, &owner) == 0);
  assert(pthread_create(&other_thr, NULL, struct_owner_worker, &other) == 0);
  assert(pthread_create(&same_no_l_thr, NULL, struct_owner_worker,
			&same_no_l) == 0);
  assert(pthread_create(&same_l_thr, NULL, struct_owner_worker,
			&same_l) == 0);

  assert(wait_for_flag(&ready, 4, 1000000));
  store_flag(&owner_start, 1);
  assert(wait_for_flag(&owner_entered, 1, 1000000));

  store_flag(&other_start, 1);
  assert(wait_for_flag(&other_entered, 1, 1000000));

  store_flag(&same_no_l_start, 1);
  store_flag(&same_l_start, 1);
  assert(wait_for_count(lj_tab_test_struct_owner_no_l_waits, 1,
			1000000));
  assert(wait_for_count(lj_tab_test_struct_owner_l_waits, 1,
			1000000));
  assert(!load_flag(&same_no_l_entered));
  assert(!load_flag(&same_l_entered));
  store_flag(&owner_release, 1);
  assert(wait_for_flag(&same_no_l_entered, 1, 1000000));
  assert(wait_for_flag(&same_l_entered, 1, 1000000));

  assert(pthread_join(owner_thr, NULL) == 0);
  assert(pthread_join(other_thr, NULL) == 0);
  assert(pthread_join(same_no_l_thr, NULL) == 0);
  assert(pthread_join(same_l_thr, NULL) == 0);
  assert(owner.status == 0);
  assert(other.status == 0);
  assert(same_no_l.status == 0);
  assert(same_l.status == 0);
  assert(lj_tab_test_struct_owner_l_waits() > 0);
  assert(lj_tab_test_struct_owner_no_l_waits() > 0);
}

static void assert_clear_seeds_absent(GCtab *t)
{
  cTValue *slot = lj_tab_getint(t, 1);
  assert(slot == NULL || tvisnil(slot));
  slot = lj_tab_getint(t, -1);
  assert(slot == NULL || tvisnil(slot));
}

static void exercise_resize_owner(lua_State *L, int clear)
{
  lua_State *ownerL, *otherL;
  pthread_t owner_thr, other_thr;
  ResizeCtx owner, other;
  GCtab *owner_tab;
  volatile int ready = 0;
  volatile int owner_start = 0;
  volatile int other_start = 0;
  volatile int owner_done = 0;
  volatile int other_done = 0;
  volatile int hook_entered = 0;
  volatile int hook_release = 0;

  lua_settop(L, 0);
  lua_createtable(L, 3, 2);  /* colocated array and hash table A */
  owner_tab = tabV(L->top - 1);
  assert(lj_tab_colo_acq(owner_tab) > 0);
  if (clear) {
    lua_pushinteger(L, 11);
    lua_rawseti(L, 1, 1);
    lua_pushinteger(L, 22);
    lua_rawseti(L, 1, -1);
  }
  lua_createtable(L, 3, 0);  /* colocated table B */

  ownerL = new_child_with_table(L, 1);
  otherL = new_child_with_table(L, clear ? 1 : 2);

  owner.L = ownerL;
  owner.ready = &ready;
  owner.start = &owner_start;
  owner.done = &owner_done;
  owner.asize = LJ_MAX_COLOSIZE + 24u;
  owner.hbits = 0;
  owner.clear = 0;
  owner.mutate = 0;
  owner.status = -1;

  other.L = otherL;
  other.ready = &ready;
  other.start = &other_start;
  other.done = &other_done;
  other.asize = LJ_MAX_COLOSIZE + 32u;
  other.hbits = 0;
  other.clear = clear;
  other.mutate = 0;
  other.status = -1;

  resize_hook_tab = owner_tab;
  resize_hook_entered = &hook_entered;
  resize_hook_release = &hook_release;
  if (clear)
    lj_tab_test_set_resize_before_freeze_hook(resize_hold_hook);
  else
    lj_tab_test_set_resize_colocated_after_freeze_hook(resize_hold_hook);

  assert(pthread_create(&owner_thr, NULL, resize_worker, &owner) == 0);
  assert(pthread_create(&other_thr, NULL, resize_worker, &other) == 0);

  assert(wait_for_flag(&ready, 2, 1000000));
  store_flag(&owner_start, 1);
  assert(wait_for_flag(&hook_entered, 1, 1000000));

  store_flag(&other_start, 1);
  assert(wait_for_flag(&other_done, 1, 1000000));
  assert(!load_flag(&owner_done));
  if (clear) {
    assert(lj_tab_struct_owner_acq(owner_tab) != 0);
    assert_clear_seeds_absent(owner_tab);
  }

  store_flag(&hook_release, 1);
  assert(wait_for_flag(&owner_done, 1, 1000000));

  assert(pthread_join(owner_thr, NULL) == 0);
  assert(pthread_join(other_thr, NULL) == 0);
  lj_tab_test_set_resize_before_freeze_hook(NULL);
  lj_tab_test_set_resize_colocated_after_freeze_hook(NULL);
  resize_hook_tab = NULL;
  resize_hook_entered = NULL;
  resize_hook_release = NULL;

  assert(owner.status == 0);
  assert(other.status == 0);
  if (clear) {
    assert(lj_tab_asize_acq(owner_tab) == owner.asize);
    assert_clear_seeds_absent(owner_tab);
  }
}

static void assert_hash_cutover_values(lua_State *L)
{
  unsigned seen = 0;
  lua_rawgeti(L, 1, -1);
  assert(lua_tointeger(L, -1) == 111);
  lua_pop(L, 1);
  lua_rawgeti(L, 1, -2);
  assert(lua_isnil(L, -1));
  lua_pop(L, 1);
  lua_rawgeti(L, 1, -3);
  assert(lua_tointeger(L, -1) == 333);
  lua_pop(L, 1);
  lua_pushnil(L);
  while (lua_next(L, 1)) {
    int key = (int)lua_tointeger(L, -2);
    unsigned bit;
    assert(key == -1 || key == -3);
    bit = 1u << -key;
    assert(!(seen & bit));
    seen |= bit;
    lua_pop(L, 1);
  }
  assert(seen == ((1u << 1) | (1u << 3)));
}

static void exercise_hash_cutover_owner(lua_State *L, int read_first)
{
  volatile int ready = 0, owner_start = 0, other_start = 0;
  volatile int owner_done = 0, other_done = 0;
  volatile int hook_entered = 0, hook_release = 0;
  ResizeCtx owner = { 0 }, other = { 0 };
  pthread_t owner_thr, other_thr;
  GCtab *t;
  Node *oldnode;

  lua_settop(L, 0);
  lua_createtable(L, 0, 4);
  t = tabV(L->top - 1);
  lua_pushinteger(L, 11);
  lua_rawseti(L, 1, -1);
  lua_pushinteger(L, 22);
  lua_rawseti(L, 1, -2);
  oldnode = lj_tab_node_acq(t);
  owner.L = new_child_with_table(L, 1);
  owner.ready = &ready;
  owner.start = &owner_start;
  owner.done = &owner_done;
  owner.hbits = 6;
  owner.status = -1;
  other.L = new_child_with_table(L, 1);
  other.ready = &ready;
  other.start = &other_start;
  other.done = &other_done;
  other.mutate = read_first ? 2 : 1;
  other.status = -1;
  resize_hook_tab = t;
  resize_hook_entered = &hook_entered;
  resize_hook_release = &hook_release;
  lj_tab_test_set_resize_hash_cutover_hook(resize_hold_hook);
  assert(pthread_create(&owner_thr, NULL, resize_worker, &owner) == 0);
  assert(pthread_create(&other_thr, NULL, resize_worker, &other) == 0);
  assert(wait_for_flag(&ready, 2, 1000000));
  store_flag(&owner_start, 1);
  assert(wait_for_flag(&hook_entered, 1, 1000000));
  assert(lj_tab_node_acq(t) == oldnode);
  assert(lj_tab_node_is_retiring(oldnode));
  assert(lj_tab_resize_desc_control_acq(t) != NULL);
  store_flag(&other_start, 1);
  assert(wait_for_flag(&other_done, 1, 1000000));
  assert(!load_flag(&owner_done));
  assert(lj_tab_node_acq(t) != oldnode);
  assert(lj_tab_resize_desc_control_acq(t) == NULL);
  assert_hash_cutover_values(L);

  store_flag(&hook_release, 1);
  assert(wait_for_flag(&owner_done, 1, 1000000));
  assert(pthread_join(owner_thr, NULL) == 0 && owner.status == 0);
  assert(pthread_join(other_thr, NULL) == 0 && other.status == 0);
  lj_tab_test_set_resize_hash_cutover_hook(NULL);
  resize_hook_tab = NULL;
  resize_hook_entered = NULL;
  resize_hook_release = NULL;
  assert_hash_cutover_values(L);
}

static void exercise_hash_cutover_weak_value(lua_State *L)
{
  GCtab *t;
  lua_settop(L, 0);
  lua_newtable(L);
  lua_newtable(L);
  lua_pushliteral(L, "v");
  lua_setfield(L, -2, "__mode");
  lua_setmetatable(L, 1);
  lua_createtable(L, 0, 4);
  t = tabV(L->top - 1);
  lua_pushinteger(L, 11);
  lua_rawseti(L, 2, -1);
  lj_tab_resize(L, t, 0, 6);
  assert(lj_tab_resize_desc_control_acq(t) == NULL);
  lua_rawseti(L, 1, 1);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_rawgeti(L, 1, 1);
  assert(lua_isnil(L, -1));
  lua_pop(L, 1);
}

static void assert_array_migration_values(lua_State *L, int weak)
{
  lua_rawgeti(L, 1, 1);
  assert(lua_isnil(L, -1));
  lua_pop(L, 1);
  lua_rawgeti(L, 1, 2);
  assert(lua_tointeger(L, -1) == 222);
  lua_pop(L, 1);
  lua_rawgeti(L, 1, 3);
  assert(weak ? lua_isnil(L, -1) : lua_istable(L, -1));
  lua_pop(L, 1);
  lua_rawgeti(L, 1, 4);
  assert(lua_isstring(L, -1));
  lua_pop(L, 1);
  lua_rawgeti(L, 1, 64);
  assert(lua_tointeger(L, -1) == 444);
  lua_pop(L, 1);
}

static void exercise_array_migration_owner(lua_State *L, int weak)
{
  volatile int ready = 0, owner_start = 0, other_start = 0;
  volatile int owner_done = 0, other_done = 0;
  volatile int hook_entered = 0, hook_release = 0;
  ResizeCtx owner = { 0 }, other = { 0 };
  pthread_t other_thr;
  GCtab *t;
  TValue *oldarray, *nextarray;

  lua_settop(L, 0);
  lua_createtable(L, LJ_MAX_COLOSIZE + 2, 0);
  if (weak) {
    lua_newtable(L);
    lua_pushliteral(L, "v");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, 1);
  }
  t = tabV(L->top - 1);
  lua_pushinteger(L, 11);
  lua_rawseti(L, 1, 1);
  lua_pushinteger(L, 22);
  lua_rawseti(L, 1, 2);
  lua_newtable(L);
  lua_rawseti(L, 1, 3);
  lua_pushliteral(L, "array migration pins string values");
  lua_rawseti(L, 1, 4);
  oldarray = lj_tab_array_acq(t);
  assert(!lj_tab_array_is_colocated(t, oldarray));
  owner.L = new_child_with_table(L, 1);
  owner.ready = &ready;
  owner.start = &owner_start;
  owner.done = &owner_done;
  owner.asize = 80;
  owner.status = -1;
  other.L = new_child_with_table(L, 1);
  other.ready = &ready;
  other.start = &other_start;
  other.done = &other_done;
  other.mutate = 3;
  other.status = -1;
  resize_hook_tab = t;
  resize_hook_entered = &hook_entered;
  resize_hook_release = &hook_release;
  lj_tab_test_set_array_copy_hook(array_copy_hold_hook);
  assert(pthread_create(&array_copy_owner, NULL, resize_worker, &owner) == 0);
  assert(pthread_create(&other_thr, NULL, resize_worker, &other) == 0);
  assert(wait_for_flag(&ready, 2, 1000000));
  store_flag(&owner_start, 1);
  assert(wait_for_flag(&hook_entered, 1, 1000000));
  assert(lj_tab_array_acq(t) == oldarray);
  assert(lj_tab_array_is_retiring(t, oldarray));
  assert(lj_tab_resize_desc_control_acq(t) != NULL);
  nextarray = lj_tab_array_nextgen_acq(oldarray);
  assert(tvisnil(&nextarray[0]));
  assert(tvisresizemarker(&nextarray[1]));

  store_flag(&other_start, 1);
  assert(wait_for_flag(&other_done, 1, 1000000));
  assert(!load_flag(&owner_done));
  assert(lj_tab_array_acq(t) == nextarray);
  assert(lj_tab_resize_desc_control_acq(t) == NULL);
  lua_rawgeti(L, 1, 1);
  assert(lua_isnil(L, -1));
  lua_pop(L, 1);

  store_flag(&hook_release, 1);
  assert(wait_for_flag(&owner_done, 1, 1000000));
  assert(pthread_join(array_copy_owner, NULL) == 0 && owner.status == 0);
  assert(pthread_join(other_thr, NULL) == 0 && other.status == 0);
  lj_tab_test_set_array_copy_hook(NULL);
  resize_hook_tab = NULL;
  resize_hook_entered = NULL;
  resize_hook_release = NULL;
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert_array_migration_values(L, weak);
}

static void exercise_resize_noop_owner(lua_State *L)
{
  global_State *g = G(L);
  GCtab *t;
  TValue *array;
  MSize asize, hmask;
  uint32_t hbits;

  lua_settop(L, 0);
  lua_newtable(L);
  lua_pushinteger(L, 1);
  lua_setfield(L, -2, "a");
  lua_pushinteger(L, 2);
  lua_setfield(L, -2, "b");
  t = tabV(L->top - 1);

  asize = lj_tab_array_snapshot_acq(t, &array);
  (void)array;
  (void)lj_tab_node_snapshot_acq(t, &hmask);
  assert(hmask > 0);
  hbits = lj_fls((uint32_t)hmask) + 1u;

  lj_tab_test_reset_struct_enter_acquires();
  lj_tab_resize(L, t, (uint32_t)asize, hbits);
  assert(lj_tab_test_struct_enter_acquires() == 0);

  lua_pushnil(L);
  lua_setfield(L, -2, "b");
  lj_tab_test_reset_struct_enter_acquires();
  lj_tab_resize(L, t, (uint32_t)asize, hbits);
  assert(lj_tab_test_struct_enter_acquires() == 0);

  lua_pushnil(L);
  lua_setfield(L, -2, "a");
  lj_tab_test_reset_struct_enter_acquires();
  assert(mt_entering_add_rlx(g, 1) == 0);
  lj_tab_resize(L, t, (uint32_t)asize, hbits);
  assert(mt_entering_sub_acqrel(g, 1) == 1);
  mt_entering_futex_wake(g, INT_MAX);
  assert(lj_tab_test_struct_enter_acquires() == 1);

  lj_tab_test_reset_struct_enter_acquires();
  lj_tab_resize(L, t, (uint32_t)asize, hbits);
  assert(lj_tab_test_struct_enter_acquires() == 0);
}

static void exercise_wait_stopreq(lua_State *L)
{
  TGState *tg = L2TG(L);
  int rc;

  lua_pushcfunction(L, tab_wait_l_lua);
  lua_setglobal(L, "lj_m5_tab_wait_l");
  lua_pushcfunction(L, publish_stopreq_and_tab_wait_l_lua);
  lua_setglobal(L, "lj_m5_publish_stopreq_and_tab_wait_l");

  ljt_tg_set_stopreq(tg);
  rc = luaL_dostring(L,
    "local ok, err = pcall(lj_m5_tab_wait_l)\n"
    "assert(ok, tostring(err))\n");
  assert_lua_ok(L, rc);
  assert(ljt_tg_has_stopreq(tg));
  ljt_tg_clear_stopreq(tg);

  rc = luaL_dostring(L,
    "local ok, err = pcall(lj_m5_publish_stopreq_and_tab_wait_l)\n"
    "assert(not ok, 'fresh STOPREQ did not interrupt table wait')\n"
    "assert(tostring(err):find('thread interrupted: VM shutdown', 1, true),\n"
    "       tostring(err))\n");
  assert_lua_ok(L, rc);
  assert(ljt_tg_has_stopreq(tg));
  ljt_tg_clear_stopreq(tg);
}

int main(void)
{
  lua_State *L = luaL_newstate();

  assert(L != NULL);
  luaL_openlibs(L);

  exercise_resize_noop_owner(L);
  exercise_direct_owner(L);
  exercise_resize_owner(L, 0);
  exercise_resize_owner(L, 1);
  exercise_hash_cutover_owner(L, 0);
  exercise_hash_cutover_owner(L, 1);
  exercise_hash_cutover_weak_value(L);
  exercise_array_migration_owner(L, 0);
  exercise_array_migration_owner(L, 1);
  exercise_wait_stopreq(L);

  lua_close(L);
  printf("t-tab-struct-owner OK: table structure ownership is per-table\n");
  return 0;
}
