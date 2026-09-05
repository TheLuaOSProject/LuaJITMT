/*
** Focused regression test for M5 table hash-vector pointer publication.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_gc2.h"
#include "lj_state.h"
#include "lj_tab.h"

static void clear_body_mark(void *p)
{
  GCArena *a = lj_arena_of(p);
  uint32_t cell = lj_arena_cellof(p);
  assert(!lj_arena_ishuge(a));
  lj_arena_bm_clear(a->mark, cell);
  assert(!lj_arena_bm_get(a->mark, cell));
}

static void assert_body_marked(void *p)
{
  GCArena *a = lj_arena_of(p);
  assert(!lj_arena_ishuge(a));
  assert(lj_arena_bm_get(a->mark, lj_arena_cellof(p)));
}

static TabNodeRetire *find_retired_node(global_State *g, Node *node)
{
  TabNodeRetire *ret;
  for (ret = lj_tab_node_retired_head_acq(g); ret != NULL;
       ret = lj_tab_node_retired_next_acq(ret))
    if (lj_tab_node_retired_node_acq(ret) == node)
      return ret;
  return NULL;
}

static TabArrayRetire *find_retired_array(global_State *g, TValue *array)
{
  TabArrayRetire *ret;
  for (ret = lj_tab_array_retired_head_acq(g); ret != NULL;
       ret = lj_tab_array_retired_next_acq(ret))
    if (lj_tab_array_retired_array_acq(ret) == array)
      return ret;
  return NULL;
}

static void check_idle_writer_publication(lua_State *L)
{
  global_State *g = G(L);
  LJGC2ActivationSnap before, after;
  GCtab *t;
  Node *node;
  TValue *array;
  void *nodebody, *arraybody;

  lua_settop(L, 0);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_smr_readers_acq(g) == 0);
  assert(lj_gc2_test_idle_reclaim_enter(g));
  assert(gc2_smr_reclaiming_acq(g) != 0);
  before = lj_gc2_activation_snapshot(&g->gc2.activation);

  /* Both side vectors are private throughout allocation and initialization;
  ** lua_createtable release-publishes their roots only after these tactical
  ** raw-body markers have returned. */
  lua_createtable(L, LJ_MAX_COLOSIZE + 8, 8);
  t = tabV(L->top-1);
  node = lj_tab_node_acq(t);
  array = lj_tab_array_acq(t);
  assert(node && node != &g->nilnode);
  assert(array && !lj_tab_array_is_colocated(t, array));
  nodebody = lj_tab_node_hdrw(node);
  arraybody = lj_tab_array_hdrw(array);

  after = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(lj_gc2_activation_equal(&before, &after));
  assert(gc2_smr_reclaiming_acq(g) != 0);
  assert(gc2_smr_readers_acq(g) == 0);

  /* Force both already-published generations white, then prove the mandatory
  ** root scan, rather than the tactical publication hint, owns retention. */
  clear_body_mark(nodebody);
  clear_body_mark(arraybody);
  lj_gc2_test_idle_reclaim_leave(g);
  assert(gc2_smr_reclaiming_acq(g) == 0);
  lj_gc2_test_scan_roots(g, L);
  assert_body_marked(nodebody);
  assert_body_marked(arraybody);
  lua_settop(L, 0);
}

static void check_idle_writer_retire_arm(lua_State *L)
{
  global_State *g = G(L);
  LJGC2ActivationSnap before, after;
  GCtab *t;
  Node *oldnode;
  TValue *oldarray;
  TabNodeRetire *nret;
  TabArrayRetire *aret;
  MSize oldasize;

  lua_settop(L, 0);
  lua_createtable(L, LJ_MAX_COLOSIZE + 8, 8);
  t = tabV(L->top-1);
  oldnode = lj_tab_node_acq(t);
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  assert(oldnode && oldnode != &g->nilnode);
  assert(oldarray && !lj_tab_array_is_colocated(t, oldarray));

  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_smr_readers_acq(g) == 0);
  assert(lj_gc2_test_idle_reclaim_enter(g));
  before = lj_gc2_activation_snapshot(&g->gc2.activation);
  lj_tab_resize(L, t, oldasize + 32u, 4);
  after = lj_gc2_activation_snapshot(&g->gc2.activation);
  assert(lj_gc2_activation_equal(&before, &after));
  assert(gc2_smr_reclaiming_acq(g) != 0);
  assert(gc2_smr_readers_acq(g) == 0);
  assert(lj_tab_node_acq(t) != oldnode);
  assert(lj_tab_array_acq(t) != oldarray);

  nret = find_retired_node(g, oldnode);
  aret = find_retired_array(g, oldarray);
  assert(nret && lj_tab_node_retired_armed_acq(nret));
  assert(aret && lj_tab_array_retired_armed_acq(aret));

  /* The published retire list owns both records and old generations. An
  ** unarmed detached consumer can only requeue; after arm, the just-published
  ** epoch cannot satisfy the grace delay in that writer pass. */
  clear_body_mark(nret);
  clear_body_mark(lj_tab_node_hdrw(oldnode));
  clear_body_mark(aret);
  clear_body_mark(lj_tab_array_hdrw(oldarray));
  lj_gc2_test_idle_reclaim_leave(g);
  assert(gc2_smr_reclaiming_acq(g) == 0);

  /* The mandatory retired-list scan remains fail-closed and must recover all
  ** four marks after the transient publication hints were skipped. */
  lj_gc2_test_scan_roots(g, L);
  assert_body_marked(nret);
  assert_body_marked(lj_tab_node_hdrw(oldnode));
  assert_body_marked(aret);
  assert_body_marked(lj_tab_array_hdrw(oldarray));
  lua_settop(L, 0);
}

static void assert_clear_hash(Node *node, MSize hmask)
{
  MSize i;
  for (i = 0; i <= hmask; i++) {
    assert(lj_tab_nextnode_acq(&node[i]) == NULL);
    assert(tvisnil(&node[i].key));
    assert(tvisnil(&node[i].val));
  }
}

static void set_pair(lua_State *L, int i)
{
  char key[32];
  snprintf(key, sizeof(key), "tab-node-publish-%02d", i);
  lua_pushlstring(L, key, strlen(key));
  lua_pushinteger(L, i + 200);
  lua_rawset(L, -3);
}

static void check_pair(lua_State *L, int i)
{
  char key[32];
  snprintf(key, sizeof(key), "tab-node-publish-%02d", i);
  lua_pushlstring(L, key, strlen(key));
  lua_rawget(L, -2);
  assert(lua_tointeger(L, -1) == i + 200);
  lua_pop(L, 1);
}

static void check_gc_key(lua_State *L)
{
  int tabidx;
  lua_settop(L, 0);
  lua_createtable(L, 0, 1);  /* target table */
  tabidx = lua_gettop(L);
  lua_newtable(L);  /* collectable key */
  lua_pushvalue(L, -1);
  lua_pushliteral(L, "gc-key-value");
  lua_rawset(L, tabidx);
  lua_pushvalue(L, -1);
  lua_rawget(L, tabidx);
  assert(strcmp(lua_tostring(L, -1), "gc-key-value") == 0);
  lua_pop(L, 1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  GCtab *t;
  Node *oldnode, *node;
  int i;

  assert(L != NULL);

  check_idle_writer_publication(L);
  check_idle_writer_retire_arm(L);

  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  assert(t->hmask == 7);
  node = lj_tab_node_acq(t);
  assert(node == noderef(t->node));
  assert(lj_tab_node_hmask_acq(node) == 7);
  assert_clear_hash(node, lj_tab_node_hmask_acq(node));

  for (i = 0; i < 6; i++)
    set_pair(L, i);
  oldnode = lj_tab_node_acq(t);
  assert(lj_tab_node_hmask_acq(oldnode) == t->hmask);
  lj_tab_resize(L, t, t->asize, lj_fls(t->hmask) + 2u);
  assert(lj_tab_node_acq(t) != oldnode);
  assert(lj_tab_node_hmask_acq(oldnode) == 7);
  assert(lj_tab_node_hmask_acq(lj_tab_node_acq(t)) == t->hmask);
  for (i = 0; i < 6; i++)
    check_pair(L, i);

  lua_settop(L, 0);
  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  oldnode = lj_tab_node_acq(t);
  assert(oldnode != &G(L)->nilnode);
  lj_tab_resize(L, t, t->asize, 0);
  assert(t->hmask == 0);
  assert(lj_tab_node_acq(t) == &G(L)->nilnode);
  assert(lj_tab_node_hmask_acq(lj_tab_node_acq(t)) == 0);

  check_gc_key(L);

  lua_close(L);
  printf("t-tab-node-publish OK: table node vectors publish with acquire/release helpers\n");
  return 0;
}
