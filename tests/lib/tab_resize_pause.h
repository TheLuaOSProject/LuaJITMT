/* Exercise a Lua reader while another thread has migrated both table vectors
** and paused immediately before publishing the successor roots. */
#ifndef TESTS_LIB_TAB_RESIZE_PAUSE_H
#define TESTS_LIB_TAB_RESIZE_PAUSE_H

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_bc.h"
#include "lj_thr.h"
#include "tab_forward_helpers.h"

typedef struct PausedResize {
  lua_State *peer;
  GCtab *table;
  MSize target;
  uint32_t paused, resume;
} PausedResize;

static PausedResize *active;

static void pause_cutover(lua_State *L, GCtab *t, TValue *array, MSize size)
{
  UNUSED(L); UNUSED(array); UNUSED(size);
  if (t == active->table) {
    la_store32_rel(&active->paused, 1);
    while (!la_load32_acq(&active->resume))
      sched_yield();
  }
}

static void *resize_table(void *ud)
{
  PausedResize *run = (PausedResize *)ud;
  assert(lj_threading_attach(run->peer));
  lj_tab_resize(run->peer, run->table, run->target, 7);
  lj_threading_detach(run->peer, 1);
  return NULL;
}

static void run_reader(lua_State *L)
{
  int status;
  lua_pushvalue(L, 2);  /* reader */
  lua_pushvalue(L, 3);  /* table */
  lua_pushvalue(L, 1);  /* expected contents */
  status = lua_pcall(L, 2, 0, 0);
  if (status)
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
  assert(status == 0);
}

static void exercise_resize(const char *reader, BCOp op, MSize initial, MSize target)
{
  lua_State *L = luaL_newstate();
  PausedResize run = {0};
  pthread_t issuer;
  GCproto *pt;
  Node *oldnode;
  TValue *oldarray, *field;
  GCstr *key;
  BCPos i;
  assert(L);
  luaL_openlibs(L);
  assert(luaL_dostring(L,
    "jit.off(); jit.flush(); collectgarbage('stop')\n"
    "local t = {[0]='zero', [64]={}, [-1]=false, field=17}\n"
    "for i=1,63 do t[i]=i*11 end; return t") == 0);
  assert(luaL_dostring(L, reader) == 0);
  pt = funcproto(funcV(L->top - 1));
  for (i = 1; i < pt->sizebc && bc_op(proto_bc(pt)[i]) != op; i++)
    ;
  assert(i < pt->sizebc);
  lua_createtable(L, (int)initial, 64);
  run.table = tabV(L->top - 1);
  lua_pushnil(L);
  while (lua_next(L, 1)) {
    lua_pushvalue(L, -2);
    lua_insert(L, -2);
    lua_rawset(L, 3);
  }
  key = tabfwd_newstr(L, "field");
  oldnode = lj_tab_node_acq(run.table);
  oldarray = lj_tab_array_acq(run.table);
  field = tabfwd_find_str_slot(oldnode, lj_tab_node_hmask_acq(oldnode), key);
  assert(field && lj_tab_array_separated(run.table));
  assert((64u < lj_tab_array_hdr_asize_acq(oldarray)) == (64u < initial));

  run.peer = lua_newthread(L);
  lua_pushvalue(L, 3);
  lua_xmove(L, run.peer, 1);
  run.target = target;
  active = &run;
  lj_tab_test_set_resize_hash_cutover_hook(pause_cutover);
  assert(pthread_create(&issuer, NULL, resize_table, &run) == 0);
  while (!la_load32_acq(&run.paused))
    (void)lj_thr_retry_yield(L);
  /* Both retiring vectors are still current when Lua first reads the table. */
  assert(lj_tab_resize_desc_control_acq(run.table));
  assert(lj_tab_array_acq(run.table) == oldarray);
  assert(lj_tab_array_is_retiring(run.table, oldarray));
  assert(lj_tab_node_acq(run.table) == oldnode);
  assert(lj_tab_node_is_retiring(oldnode));
  tabfwd_assert_forward(field);
  run_reader(L);
  la_store32_rel(&run.resume, 1);
  assert(pthread_join(issuer, NULL) == 0);
  lj_tab_test_set_resize_hash_cutover_hook(NULL);
  active = NULL;
  assert(lj_tab_resize_desc_control_acq(run.table) == NULL);
  assert(lj_tab_array_acq(run.table) != oldarray);
  assert(lj_tab_node_acq(run.table) != oldnode);
  /* Separated storage carries its own iteration bound. */
  lj_tab_asize_rel(run.table, 0);
  run_reader(L);
  lj_tab_asize_rel(run.table, target);
  lua_close(L);
}

static void exercise_resize_reads(const char *reader, BCOp op)
{
  exercise_resize(reader, op, 40, 96);
  exercise_resize(reader, op, 96, 40);
}

#endif
