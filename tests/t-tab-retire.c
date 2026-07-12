/*
** Focused regression test for M5 table hash-vector SMR retirement.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_gc2.h"
#include "lj_tab.h"
#include "lj_tg.h"
#include "lj_thr.h"

#ifndef LJ_TAB_TEST_HELPERS
#error "t-tab-retire requires LJ_TAB_TEST_HELPERS"
#endif

static TabNodeRetire *find_retired(global_State *g, Node *node)
{
  TabNodeRetire *ret;
  for (ret = lj_tab_node_retired_head_acq(g);
       ret != NULL;
       ret = lj_tab_node_retired_next_acq(ret))
    if (lj_tab_node_retired_node_acq(ret) == node)
      return ret;
  return NULL;
}

static void set_pair(lua_State *L, int i)
{
  char key[32];
  snprintf(key, sizeof(key), "tab-retire-%02d", i);
  lua_pushlstring(L, key, strlen(key));
  lua_pushinteger(L, i + 100);
  lua_rawset(L, -3);
}

static void check_pair(lua_State *L, int i)
{
  char key[32];
  snprintf(key, sizeof(key), "tab-retire-%02d", i);
  lua_pushlstring(L, key, strlen(key));
  lua_rawget(L, -2);
  assert(lua_tointeger(L, -1) == i + 100);
  lua_pop(L, 1);
}

static void set_int_pair(lua_State *L, int key, int val)
{
  lua_pushinteger(L, key);
  lua_pushinteger(L, val);
  lua_rawset(L, -3);
}

static void check_int_value(lua_State *L, int key, int val)
{
  lua_rawgeti(L, -1, key);
  assert(lua_tointeger(L, -1) == val);
  lua_pop(L, 1);
}

static void check_int_array(lua_State *L, GCtab *t, int key, int val)
{
  assert((MSize)key < lj_tab_asize_acq(t));
  assert(!lj_tv_isnil_acq(&lj_tab_array_acq(t)[key]));
  check_int_value(L, key, val);
}

static void test_table_candidate(global_State *g, GCtab *t)
{
  GCobj *bad = (GCobj *)(uintptr_t)U64x(00004000,00000000);
  assert(lj_tab_test_table_candidate(g, obj2gco(t)) == 1);
  assert(lj_tab_test_table_candidate(g, bad) == 0);
}

static int constructor_hook_seen;

static void constructor_prepublish_gc(lua_State *L, GCtab *t)
{
  GCArena *a = lj_arena_of(t);
  TValue *array;
  Node *node;
  MSize asize, hmask;
  uint32_t cell = lj_arena_cellof(t);
  assert(!lj_arena_ishuge(a));
  assert(lj_arena_ready_get(a, cell) == 1);
  asize = lj_tab_array_snapshot_acq(t, &array);
  node = lj_tab_node_snapshot_acq(t, &hmask);
  assert(asize == 65u && array != NULL);
  assert(hmask == 15u && node != NULL);
  assert(lj_tv_isnil_acq(&array[0]));
  assert(lj_tv_isnil_acq(&array[64]));
  assert(lj_tv_isnil_acq(&node[0].key));
  assert(lj_tv_isnil_acq(&node[0].val));
  /* A complete collection after the semantic anchor, READY publication and
  ** root barrier, but before ownership-chain publication, must retain both
  ** the complete table and its side vectors. Production performs no poll at
  ** the earlier READY=0/nil-anchor boundary. */
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  assert(lj_arena_ready_get(a, cell) == 1);
  assert(lj_tab_array_acq(t) == array);
  assert(lj_tab_node_acq(t) == node);
  assert(lj_gc2_mem_registered_known(G(L), lj_tab_array_hdrw(array)) == 1);
  assert(lj_gc2_mem_registered_known(G(L), lj_tab_node_hdrw(node)) == 1);
  assert(lj_gc2_ismarkedmem(G(L), lj_tab_array_hdrw(array)) == 1);
  assert(lj_gc2_ismarkedmem(G(L), lj_tab_node_hdrw(node)) == 1);
  assert(lj_tv_isnil_acq(&array[0]));
  assert(lj_tv_isnil_acq(&array[64]));
  assert(lj_tv_isnil_acq(&node[0].key));
  assert(lj_tv_isnil_acq(&node[0].val));
  constructor_hook_seen++;
}

static void test_constructor_ready_boundary(lua_State *L)
{
  GCtab *t;
  constructor_hook_seen = 0;
  lj_tab_test_set_constructor_prepublish_hook(constructor_prepublish_gc);
  lua_createtable(L, 64, 16);
  lj_tab_test_set_constructor_prepublish_hook(NULL);
  assert(constructor_hook_seen == 1);
  t = tabV(L->top - 1);
  assert(lj_arena_ready_get(lj_arena_of(t), lj_arena_cellof(t)) == 1);
  assert(lj_tab_asize_acq(t) == 65u);
  assert(lj_tab_hmask_acq(t) == 15u);
  lua_pop(L, 1);
}

static int pin_then_error(lua_State *L)
{
  lj_tab_read_enter(L2TG(L));
  return luaL_error(L, "table pin unwind fixture");
}

static void test_pin_error_unwind(lua_State *L)
{
  TGState *tg = L2TG(L);
  uint64_t outer_epoch;
  assert(lj_tg_tab_read_depth_acq(tg) == 0);
  lua_pushcfunction(L, pin_then_error);
  assert(lua_pcall(L, 0, 0, 0) != LUA_OK);
  assert(lj_tg_tab_read_depth_acq(tg) == 0);
  lua_pop(L, 1);

  /* A protected call may be nested inside an outer raw scan. Its error drops
  ** only the inner leaked scope; the caller's exact depth/epoch must survive. */
  lj_tab_read_enter(tg);
  outer_epoch = lj_tg_tab_read_epoch_acq(tg);
  lua_pushcfunction(L, pin_then_error);
  assert(lua_pcall(L, 0, 0, 0) != LUA_OK);
  assert(lj_tg_tab_read_depth_acq(tg) == 1);
  assert(lj_tg_tab_read_epoch_acq(tg) == outer_epoch);
  lua_pop(L, 1);
  lj_tab_read_leave(tg);
  assert(lj_tg_tab_read_depth_acq(tg) == 0);
}

#if LJ_HASBUFFER
static uint32_t lua_pcall_expected_pin_depth;
static uint64_t lua_pcall_expected_pin_epoch;

static int check_lua_pcall_pin_baseline(lua_State *L)
{
  TGState *tg = L2TG(L);
  assert(lj_tg_tab_read_depth_acq(tg) == lua_pcall_expected_pin_depth);
  assert(lj_tg_tab_read_epoch_acq(tg) == lua_pcall_expected_pin_epoch);
  return 0;
}

static void run_lua_serializer_pcall(lua_State *L)
{
  static const char chunk[] =
    "local ok = pcall(buffer.encode, {function() end})\n"
    "assert(not ok)\n"
    "check_lua_pcall_pin_baseline()\n";
  int status = luaL_dostring(L, chunk);
  if (status != LUA_OK) {
    fprintf(stderr, "Lua serializer pcall regression failed: %s\n",
            lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static void test_lua_fast_pcall_pin_unwind(lua_State *L)
{
  TGState *tg = L2TG(L);
  uint64_t outer_epoch;
  assert(lj_tg_tab_read_depth_acq(tg) == 0);
  luaL_openlibs(L);
  lua_pushcfunction(L, check_lua_pcall_pin_baseline);
  lua_setglobal(L, "check_lua_pcall_pin_baseline");
  assert(luaopen_string_buffer(L) == 1);
  lua_setglobal(L, "buffer");

  lua_pcall_expected_pin_depth = 0;
  lua_pcall_expected_pin_epoch = 0;
  run_lua_serializer_pcall(L);
  assert(lj_tg_tab_read_depth_acq(tg) == 0);
  assert(lj_tg_tab_read_epoch_acq(tg) == 0);

  /* The serializer's nested protected boundary must discard only its own
  ** recursive scan pins when a Lua fast pcall catches the error. */
  lj_tab_read_enter(tg);
  outer_epoch = lj_tg_tab_read_epoch_acq(tg);
  lua_pcall_expected_pin_depth = 1;
  lua_pcall_expected_pin_epoch = outer_epoch;
  run_lua_serializer_pcall(L);
  assert(lj_tg_tab_read_depth_acq(tg) == 1);
  assert(lj_tg_tab_read_epoch_acq(tg) == outer_epoch);
  lj_tab_read_leave(tg);
  assert(lj_tg_tab_read_depth_acq(tg) == 0);
  assert(lj_tg_tab_read_epoch_acq(tg) == 0);
}
#endif

static void test_mt_epoch_pin(lua_State *L)
{
  global_State *g = G(L);
  TGState extra;
  GCtab *t;
  Node *oldnode;
  MSize oldhmask;
  TabNodeRetire *ret;
  uint64_t retire_epoch;

  lj_tg_init_thread(g, &extra, NULL, 0);
  lj_tg_tid_rel(&extra, lj_thr_newid());
  lj_arena_alloc_owner_rel(&extra.alloc, lj_tg_tid_acq(&extra));
  lj_tg_attach(g, &extra);
  assert(gc2_n_threads_acq(g) == 2u);

  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  set_pair(L, 20);
  oldnode = lj_tab_node_snapshot_acq(t, &oldhmask);
  assert(oldhmask > 0);

  /* Model a long C scan which acquired oldnode before retirement and survives
  ** across completed handshakes. The reader publishes only owner-local stores;
  ** reclaim must requeue rather than wait for it. */
  lj_tab_read_enter(&extra);
  lj_tab_resize(L, t, 0, lj_fls((uint32_t)oldhmask) + 2u);
  ret = find_retired(g, oldnode);
  assert(ret != NULL);
  retire_epoch = lj_tab_node_retired_epoch_acq(ret);

  /* Multi-TG direct drains without the outer metadata writer gate fail closed. */
  assert(lj_tab_reclaim_retired(g,
				retire_epoch + LJ_TAB_RETIRE_EPOCHS) == 0);
  assert(find_retired(g, oldnode) != NULL);

  gc2_smr_reclaiming_rel(g, 1);
  (void)lj_tab_reclaim_retired(g,
			      retire_epoch + LJ_TAB_RETIRE_EPOCHS);
  assert(find_retired(g, oldnode) != NULL);
  lj_tab_read_leave(&extra);
  assert(lj_tab_reclaim_retired(g,
				retire_epoch + LJ_TAB_RETIRE_EPOCHS) >= 1u);
  assert(find_retired(g, oldnode) == NULL);
  gc2_smr_reclaiming_rel(g, 0);

  lua_pop(L, 1);
  lj_tg_detach(g, &extra);
  assert(gc2_n_threads_acq(g) == 1u);
  assert(lj_tg_reclaim_dead(g) >= 1u);
  assert(lj_tg_fini_thread(g, &extra));
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  GCtab *t;
  Node *oldnode;
  Node *newnode;
  MSize oldhmask;
  uint64_t retire_epoch;
  TabNodeRetire *ret;
  int i;

  assert(L != NULL);
  g = G(L);
  assert(lj_tab_node_retired_head_acq(g) == NULL);
  test_constructor_ready_boundary(L);

  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  test_table_candidate(g, t);
  assert(t->hmask > 0);
  for (i = 0; i < 4; i++)
    set_pair(L, i);
  set_int_pair(L, 3, 777);

  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  assert(oldhmask == t->hmask);
  assert(lj_tab_node_hdr_flags_acq(oldnode) == 0);
  lj_tab_resize(L, t, 8, lj_fls(t->hmask) + 2u);
  newnode = lj_tab_node_acq(t);
  assert(newnode != oldnode);
  assert(lj_tab_node_hmask_acq(oldnode) == oldhmask);
  assert(lj_tab_node_hdr_flags_acq(oldnode) == TABNODE_FLAG_RETIRING);
  ret = find_retired(g, oldnode);
  assert(ret != NULL);
  assert(lj_tab_node_retired_hmask_acq(ret) == oldhmask);
  assert(lj_tab_node_retired_armed_acq(ret) == 1);
  retire_epoch = lj_tab_node_retired_epoch_acq(ret);
  assert(lj_tab_reclaim_retired(g, retire_epoch) == 0);
  assert(find_retired(g, oldnode) != NULL);
  assert(lj_tab_reclaim_retired(g, retire_epoch + 1u) == 0);
  assert(find_retired(g, oldnode) != NULL);
  lj_tab_node_rel(t, oldnode);
  assert(lj_tab_reclaim_retired(g, retire_epoch + LJ_TAB_RETIRE_EPOCHS) == 0);
  assert(find_retired(g, oldnode) != NULL);
  lj_tab_node_rel(t, newnode);
  assert(lj_tab_reclaim_retired(g, retire_epoch + LJ_TAB_RETIRE_EPOCHS) == 1);
  assert(find_retired(g, oldnode) == NULL);
  for (i = 0; i < 4; i++)
    check_pair(L, i);
  check_int_array(L, t, 3, 777);
  set_int_pair(L, 6, 888);
  check_int_array(L, t, 6, 888);

  oldnode = lj_tab_node_acq(t);
  assert(lj_tab_node_hdr_flags_acq(oldnode) == 0);
  lj_tab_resize(L, t, 2, 1);
  assert(lj_tab_asize_acq(t) == 2);
  assert(lj_tab_node_acq(t) != oldnode);
  assert(lj_tab_node_hdr_flags_acq(oldnode) == TABNODE_FLAG_RETIRING);
  assert(find_retired(g, oldnode) != NULL);
  for (i = 0; i < 4; i++)
    check_pair(L, i);
  check_int_value(L, 3, 777);
  check_int_value(L, 6, 888);

  test_pin_error_unwind(L);
  test_mt_epoch_pin(L);
#if LJ_HASBUFFER
  test_lua_fast_pcall_pin_unwind(L);
#endif

  lua_close(L);
  printf("t-tab-retire OK: hash vectors rebuild, route and retire cleanly\n");
  return 0;
}
