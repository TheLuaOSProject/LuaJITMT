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
#include "lib/tab_forward_helpers.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_safepoint.h"
#include "lj_tab.h"
#include "lj_thr.h"
#include "lj_trace.h"
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
  int weak_mode_change;
  int check_inline_split;
  int status;
} ResizeCtx;

static GCtab *resize_hook_tab;
static volatile int *resize_hook_entered;
static volatile int *resize_hook_release;
static volatile int late_tail_passes, late_tail_release;
static pthread_t copy_owner;
static MSize hash_install_pause_stage;
static char collision_keys[4][128];

static global_State *first_attach_g;
static volatile int first_attach_entering;
static volatile int first_attach_release;
static volatile int first_attach_done;

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
      !pthread_equal(pthread_self(), copy_owner))
    return;
  store_flag(resize_hook_entered, 1);
  while (!load_flag(resize_hook_release))
    sched_yield();
}

static void array_migration_mutate(lua_State *L, int mixed, int weak_change)
{
  if (weak_change) {
    if (weak_change < 0) {
      lua_pushnil(L);
    } else {
      lua_newtable(L);
      lua_pushliteral(L, "v");
      lua_setfield(L, -2, "__mode");
    }
    assert(lua_setmetatable(L, 1));
  }
  lua_rawgeti(L, 1, 2);
  assert(lua_tointeger(L, -1) == 22);
  lua_pop(L, 1);
  if (mixed) {
    lua_rawgeti(L, 1, 64);
    assert(lua_tointeger(L, -1) == 64);
    lua_pop(L, 1);
  }
  lua_pushnil(L);
  lua_rawseti(L, 1, 1);
  lua_pushinteger(L, 222);
  lua_rawseti(L, 1, 2);
  lua_pushinteger(L, 444);
  lua_rawseti(L, 1, 64);
}

static void hash_copy_hold_hook(GCtab *t, MSize idx)
{
  Node *node;
  if (t != resize_hook_tab || !pthread_equal(pthread_self(), copy_owner) ||
      load_flag(resize_hook_entered))
    return;
  node = lj_tab_node_acq(t);
  if (!tvisstr(&node[idx].key))
    return;
  store_flag(resize_hook_entered, 1);
  while (!load_flag(resize_hook_release))
    sched_yield();
}

static void assert_collision_value(lua_State *L, int key, int value)
{
  lua_pushstring(L, collision_keys[key]);
  lua_rawget(L, 1);
  if (value)
    assert(lua_tointeger(L, -1) == value);
  else
    assert(lua_isnil(L, -1));
  lua_pop(L, 1);
}

static void hash_migration_mutate(lua_State *L)
{
  assert_collision_value(L, 0, 11);
  lua_pushstring(L, collision_keys[0]);
  lua_pushinteger(L, 111);
  lua_rawset(L, 1);
  lua_pushstring(L, collision_keys[1]);
  lua_pushinteger(L, 222);
  lua_rawset(L, 1);
  lua_pushstring(L, collision_keys[2]);
  lua_pushnil(L);
  lua_rawset(L, 1);
  lua_pushstring(L, collision_keys[3]);
  lua_pushinteger(L, 444);
  lua_rawset(L, 1);
}

static void verify_split_public_lua(lua_State *L, int first_write)
{
  assert_lua_ok(L, luaL_loadstring(L,
    "local t, first_write = ...\n"
    "if first_write then t[65] = 'peer wrote to split table' end\n"
    "local seen = {}\n"
    "for k, v in pairs(t) do\n"
    "  assert(seen[k] == nil)\n"
    "  seen[k] = type(v) == 'table' or v\n"
    "end\n"
    "assert(seen[1] == 11 and seen[2] == 22)\n"
    "assert(seen[4] == 'array migration pins string values')\n"
    "assert(seen[-1] == t[-1] and seen[64] == t[64])\n"
    "local step, state = ipairs(t)\n"
    "local first, value = step(state, 0)\n"
    "assert(first == 1 and value == 11 and step(state, 79) == nil)\n"
    "t[65] = 'peer wrote to split table'\n"
    "assert(t[65] == 'peer wrote to split table')\n"));
  lua_pushvalue(L, 1);
  lua_pushboolean(L, first_write);
  assert_lua_ok(L, lua_pcall(L, 2, 0, 0));
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
  else if (ctx->mutate == 3 || ctx->mutate == 5) {
    if (ctx->check_inline_split)
      verify_split_public_lua(L, ctx->check_inline_split == 2);
    array_migration_mutate(L, ctx->mutate == 5, ctx->weak_mode_change);
  } else if (ctx->mutate == 4)
    hash_migration_mutate(L);
  else if (ctx->mutate == 6) {
    lj_tab_resize(L, t, ctx->asize, ctx->hbits);
    hash_cutover_mutate(L, 1);
  }
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
  ResizeCtx owner = { 0 }, other = { 0 };
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
    lj_tab_test_set_array_copy_hook(array_copy_hold_hook);

  assert(pthread_create(clear ? &owner_thr : &copy_owner, NULL,
			resize_worker, &owner) == 0);
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

  assert(pthread_join(clear ? owner_thr : copy_owner, NULL) == 0);
  assert(pthread_join(other_thr, NULL) == 0);
  lj_tab_test_set_resize_before_freeze_hook(NULL);
  lj_tab_test_set_array_copy_hook(NULL);
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

static void late_tail_hold_hook(lua_State *L, GCtab *t, TValue *oldarray,
				MSize oldasize)
{
  UNUSED(L);
  UNUSED(oldarray);
  UNUSED(oldasize);
  if (t != resize_hook_tab || !pthread_equal(pthread_self(), copy_owner))
    return;
  add_flag(&late_tail_passes, 1);
  while (!load_flag(&late_tail_release))
    sched_yield();
}

static void exercise_array_migration_owner(lua_State *L, int weak, int mixed,
					   int weak_change, int inline_split)
{
  volatile int ready = 0, owner_start = 0, other_start = 0;
  volatile int owner_done = 0, other_done = 0;
  volatile int hook_entered = 0, hook_release = 0;
  ResizeCtx owner = { 0 }, other = { 0 };
  pthread_t other_thr;
  GCtab *t;
  Node *oldnode;
  TValue *oldarray, *nextarray;
  int i;

  lua_settop(L, 0);
  lua_createtable(L, inline_split ? 5 : (mixed >= 2 ? 80 : LJ_MAX_COLOSIZE + 2),
		  mixed ? 4 : 0);
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
  if (mixed) {
    lua_pushinteger(L, 64);
    lua_rawseti(L, 1, 64);
    lua_pushinteger(L, 555);
    lua_rawseti(L, 1, -1);
  }
  if (mixed == 3) {
    lua_newtable(L);
    lua_rawseti(L, 1, 65);
    lua_pushliteral(L, "compact migration retains strings");
    lua_rawseti(L, 1, 66);
  }
  oldnode = lj_tab_node_acq(t);
  oldarray = lj_tab_array_acq(t);
  assert(lj_tab_array_is_colocated(t, oldarray) == !!inline_split);
  owner.L = new_child_with_table(L, 1);
  owner.ready = &ready;
  owner.start = &owner_start;
  owner.done = &owner_done;
  owner.asize = inline_split == 3 ? 3 : (mixed >= 2 ? 40 : 80);
  if (inline_split == 3)
    owner.hbits = 4;
  else
    owner.hbits = mixed >= 3 ? 3 : (mixed == 2 ? 8 : (inline_split == 2 ? 2 : 0));
  owner.status = -1;
  other.L = new_child_with_table(L, 1);
  other.ready = &ready;
  other.start = &other_start;
  other.done = &other_done;
  other.mutate = mixed ? 5 : 3;
  other.weak_mode_change = weak_change;
  other.check_inline_split = inline_split;
  other.status = -1;
  resize_hook_tab = t;
  resize_hook_entered = &hook_entered;
  resize_hook_release = &hook_release;
  lj_tab_test_set_array_copy_hook(array_copy_hold_hook);
  if (mixed == 4) {
    store_flag(&late_tail_passes, 0);
    store_flag(&late_tail_release, 0);
    lj_tab_test_set_resize_before_freeze_hook(late_tail_hold_hook);
  }
  assert(pthread_create(&copy_owner, NULL, resize_worker, &owner) == 0);
  assert(pthread_create(&other_thr, NULL, resize_worker, &other) == 0);
  assert(wait_for_flag(&ready, 2, 1000000));
  store_flag(&owner_start, 1);
  if (mixed == 4) {
    assert(wait_for_flag(&late_tail_passes, 1, 1000000));
    /* These accepted writes land after candidate allocation but before
    ** retirement. They exceed the compact hash and must all survive. */
    for (i = 50; i < 59; i++) {
      lua_pushinteger(L, i);
      lua_rawseti(L, 1, i);
    }
    /* Occupying an old hash slot changes the retirement CAS, so the first
    ** attempt must discard both allocated candidates and retry. */
    lua_pushinteger(L, 777);
    lua_rawseti(L, 1, -2);
    store_flag(&late_tail_release, 1);
  }
  assert(wait_for_flag(&hook_entered, 1, 1000000));
  assert(lj_tab_array_acq(t) == oldarray);
  assert(lj_tab_array_is_retiring(t, oldarray));
  assert(lj_tab_resize_desc_control_acq(t) != NULL);
  if (mixed) {
    assert(lj_tab_node_acq(t) == oldnode && lj_tab_node_is_retiring(oldnode));
  }
  if (inline_split) {
    TabResizeDesc *desc = lj_tab_resize_desc_control_acq(t);
    assert(desc->oldarray == oldarray && !desc->oldaret);
    assert(lj_tab_colo_acq(t) < 0);
    nextarray = desc->newarray;
  } else {
    nextarray = lj_tab_array_nextgen_acq(oldarray);
  }
  assert(tvisnil(&nextarray[0]));
  assert(tvisresizemarker(&nextarray[1]));

  store_flag(&other_start, 1);
  assert(wait_for_flag(&other_done, 1, 1000000));
  assert(!load_flag(&owner_done));
  assert(lj_tab_array_acq(t) == nextarray);
  if (mixed)
    assert(lj_tab_node_acq(t) != oldnode);
  assert(lj_tab_asize_acq(t) == owner.asize);
  if (mixed >= 3)
    assert(lj_tab_hmask_acq(t) == (mixed == 3 ? 7u : 63u));
  assert(lj_tab_resize_desc_control_acq(t) == NULL);
  lua_rawgeti(L, 1, 1);
  assert(lua_isnil(L, -1));
  lua_pop(L, 1);

  store_flag(&hook_release, 1);
  assert(wait_for_flag(&owner_done, 1, 1000000));
  assert(pthread_join(copy_owner, NULL) == 0 && owner.status == 0);
  assert(pthread_join(other_thr, NULL) == 0 && other.status == 0);
  lj_tab_test_set_array_copy_hook(NULL);
  if (mixed == 4)
    lj_tab_test_set_resize_before_freeze_hook(NULL);
  resize_hook_tab = NULL;
  resize_hook_entered = NULL;
  resize_hook_release = NULL;
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert_array_migration_values(L, weak_change ? weak_change > 0 : weak);
  if (inline_split) {
    lua_rawgeti(L, 1, 65);
    assert(lua_isstring(L, -1));
    lua_pop(L, 1);
  }
  if (mixed == 3) {
    lua_rawgeti(L, 1, 65);
    assert(weak ? lua_isnil(L, -1) : lua_istable(L, -1));
    lua_pop(L, 1);
    lua_rawgeti(L, 1, 66);
    assert(lua_isstring(L, -1));
    lua_pop(L, 1);
  }
  if (mixed == 4) {
    assert(load_flag(&late_tail_passes) >= 2);
    lua_rawgeti(L, 1, -2);
    assert(lua_tointeger(L, -1) == 777);
    lua_pop(L, 1);
    for (i = 50; i < 59; i++) {
      lua_rawgeti(L, 1, i);
      assert(lua_tointeger(L, -1) == i);
      lua_pop(L, 1);
    }
  }
  if (mixed) {
    lua_rawgeti(L, 1, -1);
    assert(lua_tointeger(L, -1) == 555);
    lua_pop(L, 1);
  }
}

static void exercise_hash_migration_owner(lua_State *L)
{
  volatile int ready = 0, owner_start = 0, other_start = 0;
  volatile int owner_done = 0, other_done = 0;
  volatile int hook_entered = 0, hook_release = 0;
  ResizeCtx owner = { 0 }, other = { 0 };
  pthread_t other_thr;
  GCtab *t;
  Node *oldnode;
  uint32_t seq = 0;
  int i, count = 0;

  lua_settop(L, 0);
  lua_createtable(L, 0, 8);
  t = tabV(L->top - 1);
  lua_newtable(L);
  lua_pushliteral(L, "k");
  lua_setfield(L, -2, "__mode");
  lua_setmetatable(L, 1);

  /* Keep a weak key outside the string collision chain and live through
  ** migration. Once released it must cease retaining the dependent value. */
  for (;;) {
    uint64_t raw;
    lua_newtable(L);
    raw = gcrefu_acq((L->top - 1)->gcr);
    if ((hashrot((uint32_t)raw, (uint32_t)(raw >> 32)) & 7u) >= 5u)
      break;
    lua_pop(L, 1);
  }
  lua_newtable(L);  /* Observe whether its dependent value is retained. */
  lua_newtable(L);
  lua_pushliteral(L, "v");
  lua_setfield(L, -2, "__mode");
  lua_setmetatable(L, 3);
  lua_newtable(L);
  lua_pushvalue(L, 4);
  lua_rawseti(L, 3, 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 4);
  lua_rawset(L, 1);
  lua_pop(L, 1);
  for (i = 0; i < 4; i++) {
    GCstr *key = tabfwd_find_sid_bucket(L, "hash-migration", 63, 1, &seq);
    assert(key->len < sizeof(collision_keys[i]));
    memcpy(collision_keys[i], strdata(key), key->len + 1u);
    lua_pushstring(L, collision_keys[i]);
    if (i < 3) {
      if (i == 2)
	lua_pushvalue(L, -1);  /* Keep the deleted collision key valid for next(). */
      lua_pushinteger(L, (i + 1) * 11);
      lua_rawset(L, 1);
    }
  }

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
  other.mutate = 4;
  other.status = -1;
  resize_hook_tab = t;
  resize_hook_entered = &hook_entered;
  resize_hook_release = &hook_release;
  lj_tab_test_set_hash_collision_copy_hook(hash_copy_hold_hook);

  assert(pthread_create(&copy_owner, NULL, resize_worker, &owner) == 0);
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
  assert_collision_value(L, 2, 0);

  store_flag(&hook_release, 1);
  assert(wait_for_flag(&owner_done, 1, 1000000));
  assert(pthread_join(copy_owner, NULL) == 0 && owner.status == 0);
  assert(pthread_join(other_thr, NULL) == 0 && other.status == 0);
  lj_tab_test_set_hash_collision_copy_hook(NULL);
  resize_hook_tab = NULL;
  resize_hook_entered = NULL;
  resize_hook_release = NULL;

  lua_pushnil(L);
  lua_replace(L, 2);
  for (i = 0; i < 3; i++)
    lua_gc(L, LUA_GCCOLLECT, 0);
  lua_rawgeti(L, 3, 1);
  assert(lua_isnil(L, -1));
  lua_pop(L, 1);
  assert_collision_value(L, 0, 111);
  assert_collision_value(L, 1, 222);
  assert_collision_value(L, 2, 0);
  assert_collision_value(L, 3, 444);
  lua_pushnil(L);
  while (lua_next(L, 1)) {
    assert(lua_type(L, -2) == LUA_TSTRING);
    count++;
    lua_pop(L, 1);
  }
  assert(count == 3);
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

static void hash_install_hold_hook(lua_State *L, GCtab *t,
				     TValue *array, MSize installed)
{
  UNUSED(L); UNUSED(array);
  if (t != resize_hook_tab || installed != hash_install_pause_stage ||
      !pthread_equal(pthread_self(), copy_owner) || load_flag(resize_hook_entered))
    return;
  store_flag(resize_hook_entered, 1);
  while (!load_flag(resize_hook_release))
    sched_yield();
}

static void exercise_hash_install_owner(lua_State *L, MSize installed)
{
  volatile int ready = 0, owner_start = 0, other_start = 0;
  volatile int owner_done = 0, other_done = 0;
  volatile int hook_entered = 0, hook_release = 0;
  ResizeCtx owner = { 0 }, other = { 0 };
  pthread_t other_thr;
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
  other.hbits = 7;
  other.mutate = 6;
  other.status = -1;
  resize_hook_tab = t;
  resize_hook_entered = &hook_entered;
  resize_hook_release = &hook_release;
  hash_install_pause_stage = installed;
  lj_tab_test_set_hash_install_hook(hash_install_hold_hook);
  assert(pthread_create(&copy_owner, NULL, resize_worker, &owner) == 0);
  assert(pthread_create(&other_thr, NULL, resize_worker, &other) == 0);
  assert(wait_for_flag(&ready, 2, 1000000));
  store_flag(&owner_start, 1);
  assert(wait_for_flag(&hook_entered, 1, 1000000));
  assert(lj_tab_node_acq(t) == oldnode && !lj_tab_node_is_retiring(oldnode));
  if (installed)
    assert(lj_tab_resize_desc_control_acq(t) != NULL);
  else
    assert(lj_tab_struct_owner_acq(t) == 0);

  store_flag(&other_start, 1);
  assert(wait_for_flag(&other_done, 1, 1000000));
  assert(!load_flag(&owner_done));
  assert(lj_tab_node_acq(t) != oldnode && lj_tab_hmask_acq(t) == 127);
  assert(lj_tab_resize_desc_control_acq(t) == NULL);
  assert_hash_cutover_values(L);

  store_flag(&hook_release, 1);
  assert(wait_for_flag(&owner_done, 1, 1000000));
  assert(pthread_join(copy_owner, NULL) == 0 && owner.status == 0);
  assert(pthread_join(other_thr, NULL) == 0 && other.status == 0);
  lj_tab_test_set_hash_install_hook(NULL);
  resize_hook_tab = NULL;
  resize_hook_entered = NULL;
  resize_hook_release = NULL;
  assert(lj_tab_hmask_acq(t) == 127);
  assert_hash_cutover_values(L);
}

static void exercise_hash_shrink_owner(lua_State *L, int has_array,
                                       int read_first)
{
  volatile int ready = 0, owner_start = 0, other_start = 0;
  volatile int owner_done = 0, other_done = 0;
  volatile int hook_entered = 0, hook_release = 0;
  ResizeCtx owner = { 0 }, other = { 0 };
  pthread_t other_thr;
  GCtab *t;
  Node *oldnode;
  TValue *oldarray, *array;
  MSize asize, oldmask;
  uint32_t seq = 0, i;
  char keys[2][128];

  lua_settop(L, 0);
  lua_createtable(L, has_array ? 8 : 0, 128);
  t = tabV(L->top-1);
  if (has_array) {
    lua_pushinteger(L, 77);
    lua_rawseti(L, 1, 1);
  }
  lua_pushinteger(L, 11);
  lua_rawseti(L, 1, -1);
  lua_pushinteger(L, 22);
  lua_rawseti(L, 1, -2);
  for (i = 0; i < 2; i++) {
    GCstr *key = tabfwd_find_sid_bucket(L, "hash-shrink", 7, 1, &seq);
    assert(key->len < sizeof(keys[i]));
    memcpy(keys[i], strdata(key), key->len + 1u);
    lua_pushstring(L, keys[i]);
    lua_pushinteger(L, 44 + i);
    lua_rawset(L, 1);
  }
  asize = lj_tab_array_snapshot_acq(t, &oldarray);
  oldnode = lj_tab_node_snapshot_acq(t, &oldmask);
  assert(oldmask == 127 && (asize != 0) == has_array);
  owner.L = new_child_with_table(L, 1);
  owner.ready = &ready;
  owner.start = &owner_start;
  owner.done = &owner_done;
  owner.asize = asize;
  owner.hbits = 3;
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
  lj_tab_test_set_hash_collision_copy_hook(hash_copy_hold_hook);
  assert(pthread_create(&copy_owner, NULL, resize_worker, &owner) == 0);
  assert(pthread_create(&other_thr, NULL, resize_worker, &other) == 0);
  assert(wait_for_flag(&ready, 2, 1000000));
  store_flag(&owner_start, 1);
  assert(wait_for_flag(&hook_entered, 1, 1000000));
  assert(lj_tab_node_acq(t) == oldnode && lj_tab_node_is_retiring(oldnode));
  assert(lj_tab_resize_desc_control_acq(t));

  store_flag(&other_start, 1);
  assert(wait_for_flag(&other_done, 1, 1000000));
  assert(!load_flag(&owner_done));
  assert(lj_tab_node_acq(t) != oldnode && lj_tab_hmask_acq(t) == 7);
  assert(lj_tab_array_snapshot_acq(t, &array) == asize && array == oldarray);
  assert(lj_tab_resize_desc_control_acq(t) == NULL);

  assert_lua_ok(L, luaL_loadstring(L,
    "local t, has_array, first, second = ...\n"
    "assert(t[-1] == 111 and t[-2] == nil and t[-3] == 333)\n"
    "assert(t[1] == (has_array and 77 or nil))\n"
    "local seen, count = {}, 0\n"
    "for k, v in pairs(t) do assert(not seen[k]); seen[k] = v; count = count + 1 end\n"
    "assert(count == (has_array and 5 or 4))\n"
    "assert(seen[-1] == 111 and seen[-3] == 333 and seen[1] == t[1])\n"
    "assert(seen[first] == 44 and seen[second] == 45)\n"));
  lua_pushvalue(L, 1);
  lua_pushboolean(L, has_array);
  lua_pushstring(L, keys[0]);
  lua_pushstring(L, keys[1]);
  assert_lua_ok(L, lua_pcall(L, 4, 0, 0));

  store_flag(&hook_release, 1);
  assert(pthread_join(copy_owner, NULL) == 0 && owner.status == 0);
  assert(pthread_join(other_thr, NULL) == 0 && other.status == 0);
  lj_tab_test_set_hash_collision_copy_hook(NULL);
  resize_hook_tab = NULL;
  resize_hook_entered = NULL;
  resize_hook_release = NULL;
}

static void first_attach_hold_hook(global_State *g)
{
  if (g != first_attach_g)
    return;
  store_flag(&first_attach_entering, 1);
  while (!load_flag(&first_attach_release))
    sched_yield();
}

static void first_attach_cutover_hook(lua_State *L, GCtab *t,
				       TValue *array, MSize asize)
{
  TabResizeDesc *desc = lj_tab_resize_desc_control_acq(t);
  UNUSED(L); UNUSED(array); UNUSED(asize);
  assert(desc != NULL);
  assert(mt_entering_acq(first_attach_g) == 1);
  assert(mt_active_acq(first_attach_g) == 0);
  assert(mt_resize_guard_count_acq(first_attach_g) == 1);
  assert(lj_tab_resize_desc_flags_acq(desc) & TAB_RESIZE_DESC_F_VM_GUARD);
  assert(!lj_trace_hasany(first_attach_g));
  store_flag(&first_attach_release, 1);
  assert(wait_for_flag(&first_attach_done, 1, 1000000));
  assert(lj_tab_resize_desc_control_acq(t) == NULL);
  assert(mt_active_acq(first_attach_g) != 0);
  assert(mt_resize_guard_count_acq(first_attach_g) == 0);
}

static void exercise_first_attach_cutover(void)
{
  lua_State *L = luaL_newstate();
  ResizeCtx worker = { 0 };
  pthread_t thread;
  GCtab *t;
  volatile int ready = 0, start = 1;
  assert(L != NULL);
#if LJ_HASJIT
  luaL_openlibs(L);
  assert_lua_ok(L, luaL_dostring(L,
    "jit.opt.start('hotloop=1'); local n=0; "
    "for i=1,200 do n=n+i end; assert(n==20100)"));
  assert(lj_trace_hasany(G(L)));
#endif
  lua_createtable(L, 0, 4);
  t = tabV(L->top - 1);
  lua_pushinteger(L, 11);
  lua_rawseti(L, 1, -1);
  lua_pushinteger(L, 22);
  lua_rawseti(L, 1, -2);
  worker.L = new_child_with_table(L, 1);
  worker.ready = &ready;
  worker.start = &start;
  worker.done = &first_attach_done;
  worker.mutate = 2;
  worker.status = -1;

  first_attach_g = G(L);
  assert(mt_active_word_acq(first_attach_g) == 0);
  lj_threading_test_set_entering_hook(first_attach_hold_hook);
  lj_tab_test_set_resize_hash_cutover_hook(first_attach_cutover_hook);
  assert(pthread_create(&thread, NULL, resize_worker, &worker) == 0);
  assert(wait_for_flag(&first_attach_entering, 1, 1000000));
  lj_tab_resize(L, t, 0, 6);
  assert(wait_for_flag(&first_attach_done, 1, 1000000));
  assert(pthread_join(thread, NULL) == 0 && worker.status == 0);
  lj_tab_test_set_resize_hash_cutover_hook(NULL);
  lj_threading_test_set_entering_hook(NULL);
  first_attach_g = NULL;
  assert_hash_cutover_values(L);
  lua_close(L);
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
  exercise_array_migration_owner(L, 0, 0, 0, 0);
  exercise_array_migration_owner(L, 1, 0, 0, 0);
  exercise_array_migration_owner(L, 0, 1, 0, 0);
  exercise_array_migration_owner(L, 1, 1, 0, 0);
  exercise_array_migration_owner(L, 0, 2, 0, 0);
  exercise_array_migration_owner(L, 1, 2, 0, 0);
  exercise_array_migration_owner(L, 0, 1, 1, 0);
  exercise_array_migration_owner(L, 1, 2, -1, 0);
  exercise_array_migration_owner(L, 0, 3, 0, 0);
  exercise_array_migration_owner(L, 1, 3, 0, 0);
  exercise_array_migration_owner(L, 0, 4, 0, 0);
  exercise_array_migration_owner(L, 0, 0, 0, 1);
  exercise_array_migration_owner(L, 1, 1, 0, 1);
  exercise_array_migration_owner(L, 0, 0, 0, 2);
  exercise_array_migration_owner(L, 0, 0, 0, 3);
  exercise_array_migration_owner(L, 1, 1, 0, 3);
  exercise_hash_migration_owner(L);
  exercise_hash_install_owner(L, 0);
  exercise_hash_install_owner(L, 1);
  exercise_hash_shrink_owner(L, 0, 0);
  exercise_hash_shrink_owner(L, 1, 0);
  exercise_hash_shrink_owner(L, 1, 1);
  exercise_wait_stopreq(L);
  lua_close(L);

  exercise_first_attach_cutover();
  printf("t-tab-struct-owner OK: table structure ownership is per-table\n");
  return 0;
}
