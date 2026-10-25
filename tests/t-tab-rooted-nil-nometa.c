/* Focused direct contract test for the rooted nil/no-TAB_META reader. */
#include "lauxlib.h"
#include "lib/tab_forward_helpers.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_meta.h"
#include "lj_obj.h"
#include "lj_state.h"
#include "lj_tab.h"
#include "lj_tg.h"
#include "lua.h"
#include "lualib.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#if !defined(LJ_TAB_TEST_HELPERS) || !defined(LJ_GC2_TEST_HELPERS)
#error "rooted nil helper fixture needs table and GC2 test helpers"
#endif

typedef struct CleanState {
  uint32_t anchors, readers, waits_no_l, waits_l, waits_store_l;
} CleanState;

static CleanState clean_state(lua_State *L) {
  CleanState s;
  s.anchors = lj_tg_root_anchor_top_acq(L2TG(L));
  s.readers = gc2_smr_readers_acq(G(L));
  s.waits_no_l = lj_tab_test_wait_no_l_calls();
  s.waits_l = lj_tab_test_wait_l_calls();
  s.waits_store_l = lj_tab_test_store_wait_l_calls();
  assert(lj_gc2_rootdesc_snapshot(&L2TG(L)->root_desc, NULL) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
  return s;
}

static void assert_clean(lua_State *L, CleanState s) {
  assert(lj_tg_root_anchor_top_acq(L2TG(L)) == s.anchors);
  assert(gc2_smr_readers_acq(G(L)) == s.readers);
  assert(lj_tab_test_wait_no_l_calls() == s.waits_no_l);
  assert(lj_tab_test_wait_l_calls() == s.waits_l);
  assert(lj_tab_test_store_wait_l_calls() == s.waits_store_l);
  assert(lj_gc2_rootdesc_snapshot(&L2TG(L)->root_desc, NULL) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
}

static int nil_try(lua_State *L, cTValue *tab, cTValue *key, TValue *out) {
  CleanState before = clean_state(L);
  GCSize total = lj_gc_total_load(G(L));
  int ok = lj_tab_getnil_nometa_rooted_try(L, tab, key, out);
  assert_clean(L, before);
  assert(lj_gc_total_load(G(L)) == total);
  return ok;
}

static void push_empty_case(lua_State *L) {
  lua_newtable(L);
  lua_pushliteral(L, "missing");
  lua_pushliteral(L, "sentinel");
}

static void missing_and_found(lua_State *L) {
  TValue *tab, *key, *out;
  push_empty_case(L);
  tab = L->top - 3;
  key = L->top - 2;
  out = L->top - 1;
  assert(nil_try(L, tab, key, out) == 1 && tvisnil(out));
  lua_pop(L, 3);

  /* An existing array slot containing nil is also an authoritative miss. */
  lua_createtable(L, 128, 0);
  lua_pushinteger(L, 5);
  lua_pushliteral(L, "sentinel");
  tab = L->top - 3;
  key = L->top - 2;
  out = L->top - 1;
  assert(nil_try(L, tab, key, out) == 1 && tvisnil(out));
  lua_pop(L, 3);

  lua_newtable(L);
  lua_pushliteral(L, "present");
  lua_pushinteger(L, 41);
  lua_rawset(L, -3);
  lua_pushliteral(L, "present");
  lua_pushliteral(L, "sentinel");
  tab = L->top - 3;
  key = L->top - 2;
  out = L->top - 1;
  assert(nil_try(L, tab, key, out) == 0);
  assert(strcmp(lua_tostring(L, -1), "sentinel") == 0);
  lua_pop(L, 3);
}

static void aliasing_and_nil_key(lua_State *L) {
  TValue *tab, *key;
  push_empty_case(L);
  tab = L->top - 3;
  key = L->top - 2;
  assert(nil_try(L, tab, key, tab) == 1 && tvisnil(tab));
  lua_pop(L, 3);

  lua_newtable(L);
  lua_pushboolean(L, 0);
  lua_pushliteral(L, "output");
  tab = L->top - 3;
  key = L->top - 2;
  assert(nil_try(L, tab, key, key) == 1 && tvisnil(key));
  lua_pop(L, 3);

  lua_newtable(L);
  lua_pushnil(L);
  lua_pushliteral(L, "output");
  tab = L->top - 3;
  key = L->top - 2;
  assert(nil_try(L, tab, key, L->top - 1) == 1 && tvisnil(L->top - 1));
  lua_pop(L, 3);

  /* A hit must decline without overwriting an aliased key/output root. */
  lua_newtable(L);
  lua_pushliteral(L, "present");
  lua_pushinteger(L, 17);
  lua_rawset(L, -3);
  lua_pushliteral(L, "present");
  tab = L->top - 2;
  key = L->top - 1;
  {
    uint64_t old = tv_rawload_acq(key);
    assert(nil_try(L, tab, key, (TValue *)(void *)key) == 0);
    assert(tv_rawload_acq(key) == old);
  }
  lua_pop(L, 2);
}

static void existing_modes_unchanged(lua_State *L) {
  TValue *out;
  lua_pushliteral(L, "not-table");
  lua_pushliteral(L, "key");
  lua_pushliteral(L, "sentinel");
  out = L->top - 1;
  assert(lj_tab_gettv_rooted_try(L, L->top - 3, L->top - 2, out) ==
         LJ_TAB_ROOTED_GET_ABSENT);
  assert(tvisnil(out));
  lua_pop(L, 3);

  push_empty_case(L);
  out = L->top - 1;
  {
    uint64_t old = tv_rawload_acq(out);
    assert(!lj_tab_gettv_rooted_hit_try(L, L->top - 3, L->top - 2, out));
    assert(tv_rawload_acq(out) == old);
  }
  lua_pop(L, 3);
}

static void refusal_paths(lua_State *L) {
  TValue *tab, *key, *out;
  uint32_t expected = LJ_GC2_SMR_OPEN;
  /* A non-table root is a refusal and preserves the output cell. */
  lua_pushliteral(L, "not-table");
  lua_pushliteral(L, "key");
  lua_pushliteral(L, "sentinel");
  tab = L->top - 3;
  key = L->top - 2;
  out = L->top - 1;
  assert(nil_try(L, tab, key, out) == 0);
  assert(strcmp(lua_tostring(L, -1), "sentinel") == 0);
  lua_pop(L, 3);

  /* Existing FORWARD state represents an in-progress generation move. */
  lua_createtable(L, 128, 0);
  lua_pushinteger(L, 9);
  lua_rawseti(L, -2, 3);
  lua_pushinteger(L, 3);
  lua_pushliteral(L, "sentinel");
  tab = L->top - 3;
  key = L->top - 2;
  out = L->top - 1;
  {
    GCtab *t = tabV(tab);
    TValue *slot = &lj_tab_array_acq(t)[3], saved;
    assert(!lj_tab_array_is_colocated(t, lj_tab_array_acq(t)));
    lj_tv_load_acq(&saved, slot);
    tabfwd_store_forward(slot);
    assert(nil_try(L, tab, key, out) == 0);
    assert(strcmp(lua_tostring(L, -1), "sentinel") == 0);
    assert(lj_tab_getinttv_rooted_try(L, tab, 3, out) ==
           LJ_TAB_ROOTED_GET_ABSENT);
    assert(tvisnil(out));
    tv_rawstore_rel(slot, tv_rawload(&saved));
  }
  lua_pop(L, 3);

  /* Retiring paired vector generations are a structural retry, not absence. */
  lua_createtable(L, 128, 0);
  lua_pushinteger(L, 9);
  lua_rawseti(L, -2, 3);
  lua_pushinteger(L, 3);
  lua_pushliteral(L, "sentinel");
  tab = L->top - 3;
  key = L->top - 2;
  out = L->top - 1;
  {
    ptrdiff_t tabofs = savestack(L, tab), keyofs = savestack(L, key);
    ptrdiff_t outofs = savestack(L, out);
    GCtab *t = tabV(tab);
    TValue *oldarray = lj_tab_array_acq(t), *newarray;
    MSize oldasize = lj_tab_asize_acq(t);
    assert(oldarray && !lj_tab_array_is_colocated(t, oldarray));
    lj_tab_resize(L, t, (uint32_t)oldasize + 32u,
                  lj_tab_node_hmask_acq(lj_tab_node_acq(t))
                      ? lj_fls(lj_tab_node_hmask_acq(lj_tab_node_acq(t))) + 1u
                      : 0u);
    newarray = lj_tab_array_acq(t);
    assert(newarray != oldarray && lj_tab_array_is_retiring(t, oldarray));
    lj_tab_array_rel(t, oldarray);
    tab = restorestack(L, tabofs);
    key = restorestack(L, keyofs);
    out = restorestack(L, outofs);
    assert(nil_try(L, tab, key, out) == 0);
    assert(strcmp(lua_tostring(L, -1), "sentinel") == 0);
    assert(lj_tab_getinttv_rooted_try(L, tab, 3, out) ==
           LJ_TAB_ROOTED_GET_RETRY);
    assert(tvisnil(out));
    lj_tab_array_rel(t, newarray);
  }
  lua_pop(L, 3);

  /* KEYLOCK is a live hash-key construction claim and must not look absent. */
  lua_newtable(L);
  lua_pushliteral(L, "claim");
  lua_pushinteger(L, 5);
  lua_rawset(L, -3);
  lua_pushliteral(L, "claim");
  lua_pushliteral(L, "sentinel");
  tab = L->top - 3;
  key = L->top - 2;
  out = L->top - 1;
  {
    GCtab *t = tabV(tab);
    GCstr *str = strV(key);
    MSize hmask = lj_tab_node_hmask_acq(lj_tab_node_acq(t));
    Node *n = hashstr_node(lj_tab_node_acq(t), hmask, str);
    TValue saved, claim;
    assert(n != NULL && hmask != 0);
    lj_tv_load_acq(&saved, &n->key);
    assert(tvisstr(&saved) && strV(&saved) == str);
    claim.u64 = LJ_TKEYLOCK_BITS;
    tv_rawstore_rel(&n->key, tv_rawload(&claim));
    assert(nil_try(L, tab, key, out) == 0);
    assert(strcmp(lua_tostring(L, -1), "sentinel") == 0);
    tv_rawstore_rel(&n->key, tv_rawload(&saved));
  }
  lua_pop(L, 3);

  /* Admission refusal exercises the retry contract without a wait. */
  push_empty_case(L);
  tab = L->top - 3;
  key = L->top - 2;
  out = L->top - 1;
  assert(gc2_smr_reclaiming_cas(G(L), &expected, LJ_GC2_SMR_META_EXCLUSIVE));
  assert(nil_try(L, tab, key, out) == 0);
  assert(strcmp(lua_tostring(L, -1), "sentinel") == 0);
  gc2_smr_reclaiming_rel(G(L), LJ_GC2_SMR_OPEN);
  lua_pop(L, 3);
}

static void metatable_fallback(lua_State *L) {
  ptrdiff_t tabofs, keyofs, outofs;
  CleanState before;
  TValue *tab, *key, *out;
  lua_newtable(L);
  lua_newtable(L);
  lua_setmetatable(L, -2);
  lua_pushliteral(L, "missing");
  lua_pushliteral(L, "sentinel");
  tab = L->top - 3;
  key = L->top - 2;
  out = L->top - 1;
  assert(nil_try(L, tab, key, out) == 0);
  assert(strcmp(lua_tostring(L, -1), "sentinel") == 0);
  /* The fallback can allocate/wait. Keep indices, then rebase all pointers. */
  tabofs = savestack(L, tab);
  keyofs = savestack(L, key);
  outofs = savestack(L, out);
  before = clean_state(L);
  (void)lj_meta_tgettv_rooted(L, L->top - 3, L->top - 2, L->top - 1);
  assert_clean(L, before);
  tab = restorestack(L, tabofs);
  key = restorestack(L, keyofs);
  out = restorestack(L, outofs);
  assert(tab && key && out && tvisnil(out));
  lua_pop(L, 3);

  lua_newtable(L);
  lua_newtable(L);
  lua_newtable(L);
  lua_pushliteral(L, "x");
  lua_pushinteger(L, 73);
  lua_rawset(L, -3);
  lua_setfield(L, -2, "__index");
  lua_setmetatable(L, -2);
  lua_pushliteral(L, "x");
  lua_pushliteral(L, "sentinel");
  tab = L->top - 3;
  key = L->top - 2;
  out = L->top - 1;
  assert(nil_try(L, tab, key, out) == 0);
  tabofs = savestack(L, tab);
  keyofs = savestack(L, key);
  outofs = savestack(L, out);
  before = clean_state(L);
  (void)lj_meta_tgettv_rooted(L, L->top - 3, L->top - 2, L->top - 1);
  assert_clean(L, before);
  tab = restorestack(L, tabofs);
  key = restorestack(L, keyofs);
  out = restorestack(L, outofs);
  assert(tab && key && out && tvisnumber(out) && numberVnum(out) == 73);
  lua_pop(L, 3);
}

int main(void) {
  lua_State *L = luaL_newstate();
  assert(L);
  luaL_openlibs(L);
  missing_and_found(L);
  aliasing_and_nil_key(L);
  existing_modes_unchanged(L);
  refusal_paths(L);
  metatable_fallback(L);
  puts("t-tab-rooted-nil-nometa OK");
  lua_close(L);
  return 0;
}
