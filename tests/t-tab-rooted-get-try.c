/*
** Focused bounded tri-state authoritative-root table-get tests.
*/

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_obj.h"
#include "lj_state.h"
#include "lj_tab.h"
#include "lj_tg.h"

#include "lib/tab_forward_helpers.h"

#ifndef LJ_TAB_TEST_HELPERS
#error "t-tab-rooted-get-try requires LJ_TAB_TEST_HELPERS"
#endif
#ifndef LJ_GC2_TEST_HELPERS
#error "t-tab-rooted-get-try requires LJ_GC2_TEST_HELPERS"
#endif

static uint32_t close_finalizer_hits;

typedef struct CleanState {
  uint32_t readers;
  uint32_t anchors;
} CleanState;

typedef struct WaitState {
  uint32_t no_l;
  uint32_t l;
  uint32_t store_l;
} WaitState;

static uint32_t smr_readers(lua_State *L)
{
  return gc2_smr_readers_acq(G(L));
}

static CleanState clean_state(lua_State *L)
{
  CleanState state;
  state.readers = smr_readers(L);
  state.anchors = lj_tg_root_anchor_top_acq(L2TG(L));
  assert(lj_gc2_rootdesc_snapshot(&L2TG(L)->root_desc, NULL) ==
	 LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
  return state;
}

static void assert_clean(lua_State *L, CleanState state)
{
  assert(smr_readers(L) == state.readers);
  assert(lj_tg_root_anchor_top_acq(L2TG(L)) == state.anchors);
  assert(lj_gc2_rootdesc_snapshot(&L2TG(L)->root_desc, NULL) ==
	 LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
}

static WaitState wait_state(void)
{
  WaitState state;
  state.no_l = lj_tab_test_wait_no_l_calls();
  state.l = lj_tab_test_wait_l_calls();
  state.store_l = lj_tab_test_store_wait_l_calls();
  return state;
}

static void assert_wait_state(WaitState state)
{
  assert(lj_tab_test_wait_no_l_calls() == state.no_l);
  assert(lj_tab_test_wait_l_calls() == state.l);
  assert(lj_tab_test_store_wait_l_calls() == state.store_l);
}

static int bounded_gettv(lua_State *L, cTValue *tabroot,
			 cTValue *keyroot, TValue *outroot)
{
  WaitState state = wait_state();
  int status = lj_tab_gettv_rooted_try(L, tabroot, keyroot, outroot);
  assert_wait_state(state);
  return status;
}

static int bounded_getint(lua_State *L, cTValue *tabroot, int32_t key,
			  TValue *outroot)
{
  WaitState state = wait_state();
  int status = lj_tab_getinttv_rooted_try(L, tabroot, key, outroot);
  assert_wait_state(state);
  return status;
}

static int result_function(lua_State *L)
{
  lua_pushinteger(L, 12345);
  return 1;
}

static void populate(lua_State *L)
{
  lua_createtable(L, 16, 8);
  lua_pushinteger(L, 33);
  lua_rawseti(L, -2, 3);
  lua_pushliteral(L, "handler");
  lua_pushcfunction(L, result_function);
  lua_rawset(L, -3);
}

static void exercise_found_and_absent(lua_State *L)
{
  int top = lua_gettop(L);
  GCtab *t;
  TValue *nilslot;
  CleanState clean = clean_state(L);
  GCSize total0;

  populate(L);
  t = tabV(L->top - 1);
  total0 = lj_gc_total_load(G(L));

  lua_pushnil(L);
  assert(bounded_getint(L, L->top - 2, 3, L->top - 1) ==
	 LJ_TAB_ROOTED_GET_FOUND);
  assert(tvisnumber(L->top - 1) && lua_tointeger(L, -1) == 33);
  assert(lj_gc_total_load(G(L)) == total0);
  lua_pop(L, 1);

  /* An in-range array cell exists structurally, but nil is semantic absence. */
  lua_pushinteger(L, 999);
  assert(bounded_getint(L, L->top - 2, 7, L->top - 1) ==
	 LJ_TAB_ROOTED_GET_ABSENT);
  assert(tvisnil(L->top - 1));
  lua_pop(L, 1);

  /* The same classification applies to an existing hash node with nil value. */
  nilslot = lj_tab_setint(L, t, -700001);
  assert(lj_tv_isnil_acq(nilslot));
  lua_pushinteger(L, -700001);
  lua_pushinteger(L, 999);
  assert(bounded_gettv(L, L->top - 3, L->top - 2, L->top - 1) ==
	 LJ_TAB_ROOTED_GET_ABSENT);
  assert(tvisnil(L->top - 1));
  lua_pop(L, 2);

  lua_pushliteral(L, "handler");
  lua_pushnil(L);
  assert(bounded_gettv(L, L->top - 3, L->top - 2, L->top - 1) ==
	 LJ_TAB_ROOTED_GET_FOUND);
  assert(tvisfunc(L->top - 1) && iscfunc(funcV(L->top - 1)) &&
	 funcV(L->top - 1)->c.f == result_function);
  /* Remove the only table edge before collection.  The returned stack cell is
  ** now the sole semantic function root after every lease/source-vector scope
  ** has closed. */
  lua_pushnil(L);
  lua_setfield(L, top + 1, "handler");
  lua_getfield(L, top + 1, "handler");
  assert(lua_isnil(L, -1));
  lua_pop(L, 1);
  (void)lua_gc(L, LUA_GCCOLLECT, 0);
  lua_call(L, 0, 1);
  assert(lua_tointeger(L, -1) == 12345);
  lua_pop(L, 2);  /* Result and key. */

  lua_pushliteral(L, "missing");
  lua_pushboolean(L, 1);
  assert(bounded_gettv(L, L->top - 3, L->top - 2, L->top - 1) ==
	 LJ_TAB_ROOTED_GET_ABSENT);
  assert(tvisnil(L->top - 1));
  lua_pop(L, 2);

  /* Ordinary nil and NaN keys are misses, not permanent structural retries. */
  lua_pushnil(L);
  lua_pushinteger(L, 1);
  assert(bounded_gettv(L, L->top - 3, L->top - 2, L->top - 1) ==
	 LJ_TAB_ROOTED_GET_ABSENT);
  assert(tvisnil(L->top - 1));
  lua_pop(L, 2);
  lua_pushnumber(L, (lua_Number)NAN);
  lua_pushinteger(L, 1);
  assert(bounded_gettv(L, L->top - 3, L->top - 2, L->top - 1) ==
	 LJ_TAB_ROOTED_GET_ABSENT);
  assert(tvisnil(L->top - 1));
  lua_pop(L, 2);

  /* A valid non-table intermediate is definitive absence. */
  lua_pushliteral(L, "not-a-table");
  lua_pushliteral(L, "handler");
  lua_pushinteger(L, 1);
  assert(bounded_gettv(L, L->top - 3, L->top - 2, L->top - 1) ==
	 LJ_TAB_ROOTED_GET_ABSENT);
  assert(tvisnil(L->top - 1));
  lua_pop(L, 3);

  assert_clean(L, clean);
  lua_settop(L, top);
}

static void exercise_aliasing(lua_State *L)
{
  int top = lua_gettop(L);
  TValue *alias;
  CleanState clean = clean_state(L);

  populate(L);

  /* Key and output are the same stack root.  Confirmation must precede the
  ** function publication which consumes the key. */
  lua_pushliteral(L, "handler");
  alias = L->top - 1;
  assert(bounded_gettv(L, L->top - 2, alias, alias) ==
	 LJ_TAB_ROOTED_GET_FOUND);
  assert(tvisfunc(alias) && funcV(alias)->c.f == result_function);
  lua_pop(L, 1);

  /* Table and output may alias in the fixed-integer form. */
  lua_pushvalue(L, -1);
  alias = L->top - 1;
  assert(bounded_getint(L, alias, 3, alias) ==
	 LJ_TAB_ROOTED_GET_FOUND);
  assert(tvisnumber(alias) && lua_tointeger(L, -1) == 33);
  lua_pop(L, 1);

  /* The maximal generic alias uses one table TValue as parent, key and result.
  ** Keep the original table below it as the independent semantic root. */
  lua_newtable(L);
  lua_pushvalue(L, -1);
  lua_pushliteral(L, "self-value");
  lua_rawset(L, -3);
  lua_pushvalue(L, -1);
  alias = L->top - 1;
  assert(bounded_gettv(L, alias, alias, alias) ==
	 LJ_TAB_ROOTED_GET_FOUND);
  assert(tvisstr(alias) && strcmp(strVdata(alias), "self-value") == 0);
  lua_pop(L, 2);

  assert_clean(L, clean);
  lua_settop(L, top);
}

static void exercise_admission_retries(lua_State *L)
{
  int top = lua_gettop(L);
  global_State *g = G(L);
  GCtab *t;
  GCstr *key;
  GCfunc *fn;
  CleanState clean = clean_state(L);
  uint32_t expect;

  populate(L);
  t = tabV(L->top - 1);

  lua_pushinteger(L, 88);
  expect = LJ_GC2_SMR_OPEN;
  assert(gc2_smr_reclaiming_cas(g, &expect, LJ_GC2_SMR_META_EXCLUSIVE));
  assert(bounded_getint(L, L->top - 2, 3, L->top - 1) ==
	 LJ_TAB_ROOTED_GET_RETRY);
  assert(tvisnil(L->top - 1));
  gc2_smr_reclaiming_rel(g, LJ_GC2_SMR_OPEN);
  lua_pop(L, 1);
  assert_clean(L, clean);

  lj_gc2_test_stack_admission_retry_once(obj2gco(t));
  lua_pushinteger(L, 88);
  assert(bounded_getint(L, L->top - 2, 3, L->top - 1) ==
	 LJ_TAB_ROOTED_GET_RETRY);
  assert(tvisnil(L->top - 1));
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
  lua_pop(L, 1);

  lua_pushliteral(L, "handler");
  key = strV(L->top - 1);
  lj_gc2_test_stack_admission_retry_once(obj2gco(key));
  lua_pushinteger(L, 88);
  assert(bounded_gettv(L, L->top - 3, L->top - 2, L->top - 1) ==
	 LJ_TAB_ROOTED_GET_RETRY);
  assert(tvisnil(L->top - 1));
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
  lua_pop(L, 2);

  lua_pushliteral(L, "handler");
  lua_getfield(L, -2, "handler");
  fn = funcV(L->top - 1);
  lua_pop(L, 1);
  lj_gc2_test_stack_admission_retry_once(obj2gco(fn));
  lua_pushinteger(L, 88);
  assert(bounded_gettv(L, L->top - 3, L->top - 2, L->top - 1) ==
	 LJ_TAB_ROOTED_GET_RETRY);
  assert(tvisnil(L->top - 1));
  assert(lj_gc2_test_stack_admission_retry_hits() == 1u);
  lua_pop(L, 2);

  assert_clean(L, clean);
  lua_settop(L, top);
}

static void exercise_structural_retries(lua_State *L)
{
  int top = lua_gettop(L);
  GCtab *t;
  TValue saved, cursor, *slot;
  TValue *oldarray, *newarray;
  MSize oldasize;
  CleanState clean = clean_state(L);

  populate(L);
  t = tabV(L->top - 1);
  slot = &lj_tab_array_acq(t)[3];
  lj_tv_load_acq(&saved, slot);
  tabfwd_store_forward(slot);
  lua_pushinteger(L, 88);
  assert(bounded_getint(L, L->top - 2, 3, L->top - 1) ==
	 LJ_TAB_ROOTED_GET_ABSENT);
  assert(tvisnil(L->top - 1));
  tv_rawstore_rel(slot, tv_rawload(&saved));
  lua_pop(L, 1);

  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  assert(oldarray && !lj_tab_array_is_colocated(t, oldarray));
  lj_tab_resize(L, t, (uint32_t)oldasize + 32u,
		lj_tab_node_hmask_acq(lj_tab_node_acq(t)) ?
		  lj_fls(lj_tab_node_hmask_acq(lj_tab_node_acq(t))) + 1u : 0u);
  newarray = lj_tab_array_acq(t);
  assert(newarray != oldarray && lj_tab_array_is_retiring(t, oldarray));
  lj_tab_array_rel(t, oldarray);
  lua_pushinteger(L, 88);
  assert(bounded_getint(L, L->top - 2, 3, L->top - 1) ==
	 LJ_TAB_ROOTED_GET_RETRY);
  assert(tvisnil(L->top - 1));
  lj_tab_array_rel(t, newarray);
  lua_pop(L, 1);

  cursor.u32.lo = 1;
  cursor.u32.hi = LJ_KEYINDEX;
  tv_rawstore_rel(L->top, tv_rawload(&cursor));
  L->top++;
  lua_pushinteger(L, 88);
  assert(bounded_gettv(L, L->top - 3, L->top - 2, L->top - 1) ==
	 LJ_TAB_ROOTED_GET_RETRY);
  assert(tvisnil(L->top - 1));
  lua_pop(L, 2);

  assert_clean(L, clean);
  lua_settop(L, top);
}

static void exercise_owner_contract(lua_State *L, lua_State *wrong)
{
  int top = lua_gettop(L);
  TValue untouched;
  global_State *g = G(L);
  TGState *tg = g->main_tg;
  TGState *oldhint = L->tg_hint;
  CleanState clean = clean_state(L);
  uint64_t untouched_raw;

  populate(L);
  setintV(&untouched, 991);
  untouched_raw = tv_rawload(&untouched);
  assert(bounded_getint(wrong, L->top - 1, 3, &untouched) ==
	 LJ_TAB_ROOTED_GET_RETRY);
  assert(tv_rawload(&untouched) == untouched_raw);

  lua_pushnil(L);
  assert(lj_tg_load_cur_L(tg) == L);
  mt_shutdown_rel(g, 1);
  lj_tg_clearcur_L(g);
  L->tg_hint = NULL;
  assert(bounded_getint(L, L->top - 2, 3, L->top - 1) ==
	 LJ_TAB_ROOTED_GET_FOUND);
  assert(tvisnumber(L->top - 1) && lua_tointeger(L, -1) == 33);
  L->tg_hint = oldhint;
  mt_shutdown_rel(g, 0);
  lj_tg_setcur_L(g, L);
  lua_pop(L, 1);

  assert_clean(L, clean);
  lua_settop(L, top);
}

static int close_finalizer_get(lua_State *L)
{
  int top = lua_gettop(L);
  global_State *g = G(L);
  CleanState clean = clean_state(L);

  assert(mt_shutdown_acq(g) != 0);
  assert(L == mainthread_acq(g) && L2TG(L) == g->main_tg);
  lua_createtable(L, 4, 0);
  lua_pushinteger(L, 77);
  lua_rawseti(L, -2, 1);
  lua_pushnil(L);
  assert(bounded_getint(L, L->top - 2, 1, L->top - 1) ==
	 LJ_TAB_ROOTED_GET_FOUND);
  assert(tvisnumber(L->top - 1) && lua_tointeger(L, -1) == 77);
  assert_clean(L, clean);
  close_finalizer_hits++;
  lua_settop(L, top);
  return 0;
}

static void install_close_finalizer(lua_State *L)
{
  int status;
  lua_pushcfunction(L, close_finalizer_get);
  lua_setglobal(L, "rooted_get_close_finalizer");
  status = luaL_dostring(L,
    "local p = newproxy(true)\n"
    "getmetatable(p).__gc = function() rooted_get_close_finalizer() end\n"
    "_G.rooted_get_close_proxy = p\n");
  if (status != 0) {
    const char *msg = lua_tostring(L, -1);
    fprintf(stderr, "%s\n", msg ? msg : "close finalizer setup failed");
  }
  assert(status == 0);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  lua_State *wrong;
  assert(L != NULL);
  luaL_openlibs(L);
  wrong = lua_newthread(L);  /* Rooted, but not claimed by this actor. */
  assert(wrong != NULL);

  exercise_found_and_absent(L);
  exercise_aliasing(L);
  exercise_admission_retries(L);
  exercise_structural_retries(L);
  exercise_owner_contract(L, wrong);

  install_close_finalizer(L);
  lua_close(L);
  assert(close_finalizer_hits == 1u);
  puts("t-tab-rooted-get-try OK");
  return 0;
}
