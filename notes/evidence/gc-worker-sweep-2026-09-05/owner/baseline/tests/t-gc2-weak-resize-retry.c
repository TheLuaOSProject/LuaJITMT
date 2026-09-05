/*
** Weak clearing must replay a table when a resize publishes a replacement
** generation between classification and the keyed nil CAS.
*/

#ifndef LJ_GC2_TEST_HELPERS
#define LJ_GC2_TEST_HELPERS
#endif

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_atomic.h"
#include "lj_gc2.h"
#include "lj_obj.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_tg.h"

typedef enum CutoverKind {
  CUTOVER_ARRAY,
  CUTOVER_HASH
} CutoverKind;

typedef struct WeakCutover {
  CutoverKind kind;
  global_State *g;
  GCtab *parent;
  GCtab *donor;
  GCtab *value;
  TValue *oldarray;
  TValue *newarray;
  MSize oldasize;
  MSize newasize;
  MSize oldacap;
  MSize newacap;
  uint32_t old_array_word;
  int32_t array_key;
  Node *oldnode;
  Node *newnode;
  Node *oldfreetop;
  Node *newfreetop;
  MSize oldhmask;
  MSize newhmask;
  uint32_t old_node_word;
  GCstr *hash_key;
  TValue *oldslot;
  TValue *newslot;
  uint32_t fired;
} WeakCutover;

static WeakCutover cutover;

static void flush_and_drain(global_State *g, TGState *tg)
{
  (void)lj_gc2_flush_ssb(g, tg);
  (void)lj_gc2_test_ssb_drain(g);
  assert(lj_gc2_test_ssb_empty(g));
  assert(gc2_thread_scan_needscan_pending_acq(g) == 0);
}

static void set_weak_values(lua_State *L, int table_index)
{
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "v");
  lua_rawset(L, -3);
  assert(lua_setmetatable(L, table_index));
}

static Node *find_str_node(Node *node, MSize hmask, GCstr *key)
{
  MSize i;
  for (i = 0; i <= hmask; i++) {
    TValue keytv;
    lj_tv_load_acq(&keytv, &node[i].key);
    if (tvisstr(&keytv) && strV(&keytv) == key)
      return &node[i];
  }
  return NULL;
}

static void assert_tab_value(cTValue *slot, GCtab *value)
{
  TValue tv;
  lj_tv_load_acq(&tv, slot);
  assert(tvistab(&tv) && tabV(&tv) == value);
}

static void weak_clear_publish_successor(global_State *g, GCtab *parent,
					 TValue *slot, cTValue *key,
					 cTValue *val)
{
  WeakCutover *c = &cutover;
  assert(g == c->g && parent == c->parent && !c->fired);
  assert(tvistab(val) && tabV(val) == c->value);
  c->fired = 1;

  if (c->kind == CUTOVER_ARRAY) {
    assert(slot == &c->oldarray[c->array_key]);
    assert(tvisnumber(key));
    assert((tvisint(key) ? intV(key) : (int32_t)numV(key)) ==
	   c->array_key);
    assert(lj_tab_array_nextgen_acq(c->oldarray) == NULL);
    lj_tab_array_nextgen_rel(c->oldarray, c->newarray);
    lj_tab_array_hdr_flags_or_rel(c->oldarray, TABARRAY_FLAG_RETIRING);
    /* Match the replacement-array publication order used by resize. */
    lj_tab_acap_rel(parent, c->newacap);
    lj_tab_array_rel(parent, c->newarray);
    lj_tab_asize_rel(parent, c->newasize);
  } else {
    assert(c->kind == CUTOVER_HASH);
    assert(slot == c->oldslot);
    assert(tvisstr(key) && strV(key) == c->hash_key);
    assert(lj_tab_node_nextgen_acq(c->oldnode) == NULL);
    lj_tab_node_nextgen_rel(c->oldnode, c->newnode);
    lj_tab_node_hdr_flags_or_rel(c->oldnode, TABNODE_FLAG_RETIRING);
    /* Match newhpart_publish(): free cursor, node root, then table hmask. */
    setfreetop(parent, c->newnode, c->newfreetop);
    lj_tab_node_rel(parent, c->newnode);
    lj_tab_hmask_rel(parent, c->newhmask);
  }
}

static void begin_weak_clear(global_State *g, TGState *tg, GCtab *parent,
			     GCtab *value)
{
  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  flush_and_drain(g, tg);
  assert(lj_gc2_test_weak_snapshot_count(g) == 1u);
  assert(lj_gc2_test_weak_snapshot_tab(g, 0) == parent);
  assert(lj_gc2_ismarked(g, obj2gco(value)) == 0);
  lj_gc2_mark_to_weak(g);
  assert(gc2_phase_acq(g) == LJ_GC2_WEAK);
  flush_and_drain(g, tg);
  gc2_weak_mark_closed_rel(g, 1);
  assert(gc2_weak_clear_cursor_acq(g) == 0);
}

static void assert_retry_then_replay(global_State *g, cTValue *oldslot,
				     cTValue *newslot, GCtab *value)
{
  uint64_t tables0 = gc2_weak_clear_tables_acq(g);
  uint64_t cleared0 = gc2_weak_clear_cleared_acq(g);

  lj_gc2_test_weak_clear_before_cas(weak_clear_publish_successor);
  assert(lj_gc2_test_weak_snapshot_clear(g, 1) == 0);
  assert(cutover.fired == 1u);
  assert(gc2_weak_clear_cursor_acq(g) == 0);
  assert(gc2_weak_clear_tables_acq(g) == tables0);
  assert(gc2_weak_clear_cleared_acq(g) == cleared0);
  assert_tab_value(oldslot, value);
  assert_tab_value(newslot, value);

  /* The successor is now the published root. Replaying the same snapshot
  ** clears that generation and only then consumes the cursor. */
  assert(lj_gc2_test_weak_snapshot_clear(g, 1) == 1u);
  assert(gc2_weak_clear_cursor_acq(g) == 1u);
  assert(gc2_weak_clear_tables_acq(g) == tables0 + 1u);
  assert(gc2_weak_clear_cleared_acq(g) == cleared0 + 1u);
  assert(lj_tv_isnil_acq(newslot));
}

static void exercise_array_cutover(lua_State *L, global_State *g, TGState *tg)
{
  const uint32_t oldwant = LJ_MAX_COLOSIZE + 16u;
  const uint32_t newwant = LJ_MAX_COLOSIZE + 32u;
  const int32_t key = 5;
  GCtab *parent, *value, *donor;

  lua_settop(L, 0);
  lua_createtable(L, (int)oldwant, 0);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  value = tabV(L->top - 1);
  lj_tab_storetab(L, lj_tab_setint(L, parent, key), value);
  set_weak_values(L, 1);
  lua_createtable(L, (int)newwant, 0);
  donor = tabV(L->top - 1);
  lj_tab_storetab(L, lj_tab_setint(L, donor, key), value);

  memset(&cutover, 0, sizeof(cutover));
  cutover.kind = CUTOVER_ARRAY;
  cutover.g = g;
  cutover.parent = parent;
  cutover.donor = donor;
  cutover.value = value;
  cutover.oldasize = lj_tab_array_snapshot_acq(parent, &cutover.oldarray);
  cutover.newasize = lj_tab_array_snapshot_acq(donor, &cutover.newarray);
  cutover.oldacap = lj_tab_acap_acq(parent);
  cutover.newacap = lj_tab_acap_acq(donor);
  cutover.old_array_word =
    la_load32_acq(&lj_tab_array_hdr(cutover.oldarray)->acap);
  cutover.array_key = key;
  assert(cutover.oldarray && cutover.newarray &&
	 cutover.oldarray != cutover.newarray);
  assert(lj_tab_array_separated(parent) && lj_tab_array_separated(donor));
  assert((MSize)key < cutover.oldasize &&
	 (MSize)key < cutover.newasize);
  assert_tab_value(&cutover.oldarray[key], value);
  assert_tab_value(&cutover.newarray[key], value);

  begin_weak_clear(g, tg, parent, value);
  assert_retry_then_replay(g, &cutover.oldarray[key],
			   &cutover.newarray[key], value);

  /* Complete the ownership swap without adding synthetic retire records. */
  lj_tab_storenilraw(&cutover.oldarray[key]);
  lj_tab_array_nextgen_rel(cutover.oldarray, NULL);
  la_store32_rel(&lj_tab_array_hdrw(cutover.oldarray)->acap,
		 cutover.old_array_word);
  lj_tab_acap_rel(donor, cutover.oldacap);
  lj_tab_array_rel(donor, cutover.oldarray);
  lj_tab_asize_rel(donor, cutover.oldasize);
  assert(lj_tab_array_acq(parent) == cutover.newarray);
  assert(lj_tab_array_acq(donor) == cutover.oldarray);

  lj_gc2_cycle_to_idle(g);
  lua_settop(L, 0);
}

static void exercise_hash_cutover(lua_State *L, global_State *g, TGState *tg)
{
  GCtab *parent, *value, *donor;
  GCstr *key;
  TValue keytv;
  Node *oldentry, *newentry;

  lua_settop(L, 0);
  lua_createtable(L, 0, 8);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  value = tabV(L->top - 1);
  key = lj_str_newlit(L, "gc2 weak hash resize retry");
  lj_tab_storetab(L, lj_tab_setstr(L, parent, key), value);
  set_weak_values(L, 1);
  lua_createtable(L, 0, 32);
  donor = tabV(L->top - 1);
  lj_tab_storetab(L, lj_tab_setstr(L, donor, key), value);

  memset(&cutover, 0, sizeof(cutover));
  cutover.kind = CUTOVER_HASH;
  cutover.g = g;
  cutover.parent = parent;
  cutover.donor = donor;
  cutover.value = value;
  cutover.oldnode = lj_tab_node_snapshot_acq(parent, &cutover.oldhmask);
  cutover.newnode = lj_tab_node_snapshot_acq(donor, &cutover.newhmask);
  cutover.oldfreetop = getfreetop(parent, cutover.oldnode);
  cutover.newfreetop = getfreetop(donor, cutover.newnode);
  cutover.old_node_word = lj_tab_node_hdr_flags_word_acq(cutover.oldnode);
  cutover.hash_key = key;
  assert(cutover.oldnode && cutover.newnode &&
	 cutover.oldnode != cutover.newnode);
  assert(cutover.oldhmask > 0 && cutover.newhmask > cutover.oldhmask);
  oldentry = find_str_node(cutover.oldnode, cutover.oldhmask, key);
  newentry = find_str_node(cutover.newnode, cutover.newhmask, key);
  assert(oldentry != NULL && newentry != NULL);
  cutover.oldslot = &oldentry->val;
  cutover.newslot = &newentry->val;
  assert_tab_value(cutover.oldslot, value);
  assert_tab_value(cutover.newslot, value);

  begin_weak_clear(g, tg, parent, value);
  assert_retry_then_replay(g, cutover.oldslot, cutover.newslot, value);

  /* Transfer the retired-side allocation to the donor table for normal close. */
  lj_tab_storenilraw(cutover.oldslot);
  lj_tab_node_nextgen_rel(cutover.oldnode, NULL);
  la_store32_rel(&lj_tab_node_hdrw(cutover.oldnode)->flags,
		 cutover.old_node_word);
  setfreetop(donor, cutover.oldnode, cutover.oldfreetop);
  lj_tab_node_rel(donor, cutover.oldnode);
  lj_tab_hmask_rel(donor, cutover.oldhmask);
  setstrV(L, &keytv, key);
  assert(lj_tab_node_acq(parent) == cutover.newnode);
  assert(lj_tab_node_acq(donor) == cutover.oldnode);
  assert(tvisnil(lj_tab_get(L, parent, &keytv)));

  lj_gc2_cycle_to_idle(g);
  lua_settop(L, 0);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;

  assert(L != NULL);
  lua_gc(L, LUA_GCSTOP, 0);
  g = G(L);
  tg = G2TG(g);
  assert(g != NULL && tg != NULL);

  exercise_array_cutover(L, g, tg);
  exercise_hash_cutover(L, g, tg);
  lj_gc2_test_weak_clear_before_cas(NULL);
  lua_close(L);
  puts("t-gc2-weak-resize-retry OK");
  return 0;
}
