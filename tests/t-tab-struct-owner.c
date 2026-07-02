/*
** Focused guard for per-table structural ownership.
*/

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_tab.h"
#include "lj_thr.h"

#ifndef LJ_TAB_TEST_HELPERS
#error "t-tab-struct-owner requires LJ_TAB_TEST_HELPERS"
#endif

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
  int status;
} ResizeCtx;

static GCtab *resize_hook_tab;
static volatile int *resize_hook_entered;
static volatile int *resize_hook_release;

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

  lj_tab_resize(L, t, ctx->asize, 0);
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
  lua_State *ownerL, *otherL, *sameL;
  pthread_t owner_thr, other_thr, same_thr;
  WorkerCtx owner, other, same;
  volatile int ready = 0;
  volatile int owner_start = 0;
  volatile int other_start = 0;
  volatile int same_start = 0;
  volatile int owner_release = 0;
  volatile int no_release = 0;
  volatile int owner_entered = 0;
  volatile int other_entered = 0;
  volatile int same_entered = 0;

  lua_settop(L, 0);
  lua_newtable(L);  /* table A */
  lua_newtable(L);  /* table B */
  assert(lua_gettop(L) == 2);

  ownerL = new_child_with_table(L, 1);
  otherL = new_child_with_table(L, 2);
  sameL = new_child_with_table(L, 1);

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

  same.L = sameL;
  same.ready = &ready;
  same.start = &same_start;
  same.release = &no_release;
  same.entered = &same_entered;
  same.hold = 0;
  same.no_l = 1;
  same.status = -1;

  lj_tab_test_reset_struct_owner_no_l_futex_waits();

  assert(pthread_create(&owner_thr, NULL, struct_owner_worker, &owner) == 0);
  assert(pthread_create(&other_thr, NULL, struct_owner_worker, &other) == 0);
  assert(pthread_create(&same_thr, NULL, struct_owner_worker, &same) == 0);

  assert(wait_for_flag(&ready, 3, 1000000));
  store_flag(&owner_start, 1);
  assert(wait_for_flag(&owner_entered, 1, 1000000));

  store_flag(&other_start, 1);
  assert(wait_for_flag(&other_entered, 1, 1000000));

  store_flag(&same_start, 1);
  assert(!wait_for_flag(&same_entered, 1, 10000));
  store_flag(&owner_release, 1);
  assert(wait_for_flag(&same_entered, 1, 1000000));

  assert(pthread_join(owner_thr, NULL) == 0);
  assert(pthread_join(other_thr, NULL) == 0);
  assert(pthread_join(same_thr, NULL) == 0);
  assert(owner.status == 0);
  assert(other.status == 0);
  assert(same.status == 0);
  assert(lj_tab_test_struct_owner_no_l_futex_waits() > 0);
}

static void exercise_resize_owner(lua_State *L)
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
  lua_createtable(L, 3, 0);  /* colocated table A */
  owner_tab = tabV(L->top - 1);
  assert(lj_tab_colo_acq(owner_tab) > 0);
  lua_createtable(L, 3, 0);  /* colocated table B */
  assert(lua_gettop(L) == 2);

  ownerL = new_child_with_table(L, 1);
  otherL = new_child_with_table(L, 2);

  owner.L = ownerL;
  owner.ready = &ready;
  owner.start = &owner_start;
  owner.done = &owner_done;
  owner.asize = LJ_MAX_COLOSIZE + 24u;
  owner.status = -1;

  other.L = otherL;
  other.ready = &ready;
  other.start = &other_start;
  other.done = &other_done;
  other.asize = LJ_MAX_COLOSIZE + 32u;
  other.status = -1;

  resize_hook_tab = owner_tab;
  resize_hook_entered = &hook_entered;
  resize_hook_release = &hook_release;
  lj_tab_test_set_resize_colocated_after_freeze_hook(resize_hold_hook);

  assert(pthread_create(&owner_thr, NULL, resize_worker, &owner) == 0);
  assert(pthread_create(&other_thr, NULL, resize_worker, &other) == 0);

  assert(wait_for_flag(&ready, 2, 1000000));
  store_flag(&owner_start, 1);
  assert(wait_for_flag(&hook_entered, 1, 1000000));

  store_flag(&other_start, 1);
  assert(wait_for_flag(&other_done, 1, 1000000));
  assert(!load_flag(&owner_done));

  store_flag(&hook_release, 1);
  assert(wait_for_flag(&owner_done, 1, 1000000));

  assert(pthread_join(owner_thr, NULL) == 0);
  assert(pthread_join(other_thr, NULL) == 0);
  lj_tab_test_set_resize_colocated_after_freeze_hook(NULL);
  resize_hook_tab = NULL;
  resize_hook_entered = NULL;
  resize_hook_release = NULL;

  assert(owner.status == 0);
  assert(other.status == 0);
}

int main(void)
{
  lua_State *L = luaL_newstate();

  assert(L != NULL);
  luaL_openlibs(L);

  exercise_direct_owner(L);
  exercise_resize_owner(L);

  lua_close(L);
  printf("t-tab-struct-owner OK: table structure ownership is per-table\n");
  return 0;
}
