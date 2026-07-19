/*
** Focused address-only keyed table-slot resolver tests.
*/

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_gc2.h"
#include "lj_obj.h"
#include "lj_state.h"
#include "lj_tab.h"
#include "lj_tabtxn.h"
#include "lj_tg.h"
#include "lj_thr.h"

#include "lib/tab_forward_helpers.h"

#ifndef LJ_TAB_TEST_HELPERS
#error "t-tab-keyed-slot-resolver requires LJ_TAB_TEST_HELPERS"
#endif

static uintptr_t ptr_addr(const void *p)
{
  return (uintptr_t)p;
}

static uint32_t close_finalizer_resolver_hits;

static uint32_t smr_readers(lua_State *L)
{
  return gc2_smr_readers_acq(G(L));
}

static void assert_rootdesc_idle(lua_State *L)
{
  assert(lj_gc2_rootdesc_snapshot(&L2TG(L)->root_desc, NULL) ==
	 LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
}

static int close_finalizer_resolver(lua_State *L)
{
  int top = lua_gettop(L);
  global_State *g = G(L);
  cTValue *tabroot, *keyroot;
  uintptr_t addr = UINTPTR_MAX, fresh = UINTPTR_MAX;
  uint32_t readers0 = smr_readers(L);

  assert(mt_shutdown_acq(g) != 0);
  assert(L == mainthread_acq(g) && L2TG(L) == g->main_tg);
  assert_rootdesc_idle(L);
  lua_createtable(L, 0, 0);
  lua_pushinteger(L, -6666661);
  tabroot = L->top - 2;
  keyroot = L->top - 1;
  assert(lj_tab_keyed_slot_resolve_or_insert_rooted_l(
	   L, &tabroot, &keyroot, &addr) == LJ_TAB_KEYED_SLOT_FOUND);
  assert(addr != 0);
  assert(lj_tab_keyed_slot_resolve_rooted_try(
	   L, tabroot, keyroot, &fresh) == LJ_TAB_KEYED_SLOT_FOUND);
  assert(fresh == addr);
  assert(smr_readers(L) == readers0);
  assert_rootdesc_idle(L);
  close_finalizer_resolver_hits++;
  lua_settop(L, top);
  return 0;
}

static void install_close_finalizer(lua_State *L)
{
  int status;
  lua_pushcfunction(L, close_finalizer_resolver);
  lua_setglobal(L, "resolver_close_finalizer");
  status = luaL_dostring(L,
    "local p = newproxy(true)\n"
    "getmetatable(p).__gc = function() resolver_close_finalizer() end\n"
    "_G.resolver_close_proxy = p\n");
  if (status != 0) {
    const char *msg = lua_tostring(L, -1);
    fprintf(stderr, "%s\n", msg ? msg : "close finalizer setup failed");
  }
  assert(status == 0);
}

static void assert_rooted_addr(lua_State *L, cTValue *tabroot,
			       cTValue *keyroot, uintptr_t want)
{
  uintptr_t addr = UINTPTR_MAX;
  assert(lj_tab_keyed_slot_resolve_rooted_try(L, tabroot, keyroot, &addr) ==
	 LJ_TAB_KEYED_SLOT_FOUND);
  assert(addr == want && addr != 0);
}

static void exercise_array_and_absent(lua_State *L)
{
  int top = lua_gettop(L);
  GCtab *t;
  TValue *array;
  cTValue *tabroot, *keyroot;
  uintptr_t addr = UINTPTR_MAX;
  MSize asize, hmask;
  Node *node;

  lua_createtable(L, 16, 0);
  lua_pushinteger(L, 4);
  tabroot = L->top - 2;
  keyroot = L->top - 1;
  t = tabV(tabroot);
  array = lj_tab_array_acq(t);
  asize = lj_tab_asize_acq(t);
  node = lj_tab_node_acq(t);
  hmask = lj_tab_node_hmask_acq(node);
  assert(array != NULL && asize > 4 && lj_tv_isnil_acq(&array[4]));

  lj_tab_test_reset_wait_no_l_calls();
  assert(lj_tab_keyed_slot_resolve_rooted_try(L, tabroot, keyroot, &addr) ==
	 LJ_TAB_KEYED_SLOT_FOUND);
  assert(addr == ptr_addr(&array[4]));
  assert(lj_tab_array_acq(t) == array && lj_tab_asize_acq(t) == asize);
  assert(lj_tab_node_acq(t) == node &&
	 lj_tab_node_hmask_acq(node) == hmask);
  assert(lj_tab_test_wait_no_l_calls() == 0u);

  lj_tab_storeint(L, &array[4], 44);
  assert_rooted_addr(L, tabroot, keyroot, ptr_addr(&array[4]));

  lua_pop(L, 1);
  lua_pushinteger(L, 1000003);
  tabroot = L->top - 2;
  keyroot = L->top - 1;
  addr = UINTPTR_MAX;
  assert(lj_tab_keyed_slot_resolve_rooted_try(L, tabroot, keyroot, &addr) ==
	 LJ_TAB_KEYED_SLOT_ABSENT);
  assert(addr == 0);
  assert(lj_tab_array_acq(t) == array && lj_tab_asize_acq(t) == asize);
  assert(lj_tab_node_acq(t) == node &&
	 lj_tab_node_hmask_acq(node) == hmask);
  lua_settop(L, top);
}

static void exercise_hash_nil_and_insert(lua_State *L)
{
  int top = lua_gettop(L);
  GCtab *t;
  TValue *slot;
  cTValue *tabroot, *keyroot;
  uintptr_t addr = UINTPTR_MAX, fresh = UINTPTR_MAX;
  MSize asize, hmask;
  TValue *array;
  Node *node;
  const int32_t present = -100000003;
  const int32_t absent = -100000019;

  lua_createtable(L, 0, 8);
  t = tabV(L->top - 1);
  slot = lj_tab_setint(L, t, present);  /* Existing structural nil slot. */
  assert(lj_tv_isnil_acq(slot));
  lua_pushinteger(L, present);
  tabroot = L->top - 2;
  keyroot = L->top - 1;
  array = lj_tab_array_acq(t);
  asize = lj_tab_asize_acq(t);
  node = lj_tab_node_acq(t);
  hmask = lj_tab_node_hmask_acq(node);

  lj_tab_test_reset_wait_no_l_calls();
  assert(lj_tab_keyed_slot_resolve_rooted_try(L, tabroot, keyroot, &addr) ==
	 LJ_TAB_KEYED_SLOT_FOUND);
  assert(addr == ptr_addr(slot));
  assert(lj_tab_keyed_slot_resolve_rooted_try(L, tabroot, keyroot, &fresh) ==
	 LJ_TAB_KEYED_SLOT_FOUND);
  assert(fresh == addr);
  assert(lj_tab_array_acq(t) == array && lj_tab_asize_acq(t) == asize);
  assert(lj_tab_node_acq(t) == node &&
	 lj_tab_node_hmask_acq(node) == hmask);
  assert(lj_tab_test_wait_no_l_calls() == 0u);

  lua_pop(L, 1);
  lua_pushinteger(L, absent);
  tabroot = L->top - 2;
  keyroot = L->top - 1;
  addr = UINTPTR_MAX;
  assert(lj_tab_keyed_slot_resolve_rooted_try(L, tabroot, keyroot, &addr) ==
	 LJ_TAB_KEYED_SLOT_ABSENT);
  assert(addr == 0);

  assert(lj_tab_keyed_slot_resolve_or_insert_rooted_l(
	   L, &tabroot, &keyroot, &addr) == LJ_TAB_KEYED_SLOT_FOUND);
  assert(addr != 0);
  assert(lj_tab_keyed_slot_resolve_rooted_try(L, tabroot, keyroot, &fresh) ==
	 LJ_TAB_KEYED_SLOT_FOUND);
  assert(fresh == addr);
  lua_settop(L, top);
}

static void find_numeric_collision(GCtab *t, int32_t *first,
				    int32_t *second)
{
  Node *node = lj_tab_node_acq(t);
  MSize hmask = lj_tab_node_hmask_acq(node);
  int32_t a, b;
  assert(hmask > 0);
  for (a = -1000; a > -5000; a--) {
    TValue ka;
    setnumV(&ka, (lua_Number)a);
    for (b = a - 1; b > -5000; b--) {
      TValue kb;
      setnumV(&kb, (lua_Number)b);
      if (hashnum_node(node, hmask, &ka) ==
	  hashnum_node(node, hmask, &kb)) {
	*first = a;
	*second = b;
	return;
      }
    }
  }
  assert(0 && "numeric hash collision not found");
}

static void exercise_collision_insert(lua_State *L)
{
  int top = lua_gettop(L);
  int32_t first = 0, second = 0;
  GCtab *t;
  cTValue *tabroot, *keyroot;
  uintptr_t addr = UINTPTR_MAX, fresh = UINTPTR_MAX, firstaddr;

  lua_createtable(L, 0, 8);
  t = tabV(L->top - 1);
  find_numeric_collision(t, &first, &second);
  (void)lj_tab_setint(L, t, first);
  lua_pushinteger(L, first);
  tabroot = L->top - 2;
  keyroot = L->top - 1;
  assert(lj_tab_keyed_slot_resolve_rooted_try(
	   L, tabroot, keyroot, &firstaddr) == LJ_TAB_KEYED_SLOT_FOUND);
  assert(firstaddr != 0);
  lua_pop(L, 1);
  lua_pushinteger(L, second);
  tabroot = L->top - 2;
  keyroot = L->top - 1;
  assert(lj_tab_keyed_slot_resolve_or_insert_rooted_l(
	   L, &tabroot, &keyroot, &addr) == LJ_TAB_KEYED_SLOT_FOUND);
  assert(addr != 0 && addr != firstaddr);
  assert(lj_tab_keyed_slot_resolve_rooted_try(L, tabroot, keyroot, &fresh) ==
	 LJ_TAB_KEYED_SLOT_FOUND);
  assert(fresh == addr);
  lua_settop(L, top);
}

static void exercise_generic_detach_keys(lua_State *L)
{
  int top = lua_gettop(L);
  const int32_t numeric_key = -123456789;
  GCtab *t;
  cTValue *tabroot, *keyroot;
  uintptr_t addr = UINTPTR_MAX, fresh = UINTPTR_MAX;

  /* Iteration exposes integral hash keys as lua_Number values.  Detach must
  ** resolve that representation to the canonical numeric hash node. */
  lua_createtable(L, 0, 8);
  t = tabV(L->top - 1);
  (void)lj_tab_setint(L, t, numeric_key);
  lua_pushnumber(L, (lua_Number)numeric_key);
  tabroot = L->top - 2;
  keyroot = L->top - 1;
  assert(tvisnum(keyroot));
  assert(lj_tab_keyed_slot_resolve_rooted_try(
	   L, tabroot, keyroot, &addr) == LJ_TAB_KEYED_SLOT_FOUND);
  assert(addr != 0);
  assert(lj_tab_keyed_slot_resolve_rooted_try(
	   L, tabroot, keyroot, &fresh) == LJ_TAB_KEYED_SLOT_FOUND);
  assert(fresh == addr);
  lua_settop(L, top);

  lua_createtable(L, 0, 8);
  lua_pushliteral(L, "detach-string-event");
  tabroot = L->top - 2;
  keyroot = L->top - 1;
  t = tabV(tabroot);
  assert(tvisstr(keyroot));
  (void)lj_tab_setstr(L, t, strV(keyroot));
  /* The setter is allocation-capable; derive the stack root pointers again. */
  tabroot = L->top - 2;
  keyroot = L->top - 1;
  addr = fresh = UINTPTR_MAX;
  assert(lj_tab_keyed_slot_resolve_rooted_try(
	   L, tabroot, keyroot, &addr) == LJ_TAB_KEYED_SLOT_FOUND);
  assert(addr != 0);
  assert(lj_tab_keyed_slot_resolve_rooted_try(
	   L, tabroot, keyroot, &fresh) == LJ_TAB_KEYED_SLOT_FOUND);
  assert(fresh == addr);
  lua_settop(L, top);
}

static void exercise_invalid_keys(lua_State *L)
{
  int top = lua_gettop(L);
  cTValue *tabroot, *keyroot;
  cTValue *intab, *inkey;
  TValue cursor;
  uintptr_t addr;

  lua_createtable(L, 4, 0);
  lua_pushnil(L);
  tabroot = L->top - 2;
  keyroot = L->top - 1;
  addr = UINTPTR_MAX;
  assert(lj_tab_keyed_slot_resolve_rooted_try(L, tabroot, keyroot, &addr) ==
	 LJ_TAB_KEYED_SLOT_RETRY);
  assert(addr == 0);
  intab = tabroot;
  inkey = keyroot;
  addr = UINTPTR_MAX;
  assert(lj_tab_keyed_slot_resolve_or_insert_rooted_l(
	   L, &intab, &inkey, &addr) == LJ_TAB_KEYED_SLOT_RETRY);
  assert(addr == 0 && intab == tabroot && inkey == keyroot);
  assert(lj_tab_keyed_slot_resolve_rooted_try(
	   L, tabroot, keyroot, NULL) == LJ_TAB_KEYED_SLOT_RETRY);

  lua_pop(L, 1);
  lua_pushnumber(L, (lua_Number)NAN);
  tabroot = L->top - 2;
  keyroot = L->top - 1;
  addr = UINTPTR_MAX;
  assert(lj_tab_keyed_slot_resolve_rooted_try(L, tabroot, keyroot, &addr) ==
	 LJ_TAB_KEYED_SLOT_RETRY);
  assert(addr == 0);
  intab = tabroot;
  inkey = keyroot;
  addr = UINTPTR_MAX;
  assert(lj_tab_keyed_slot_resolve_or_insert_rooted_l(
	   L, &intab, &inkey, &addr) == LJ_TAB_KEYED_SLOT_RETRY);
  assert(addr == 0 && intab == tabroot && inkey == keyroot);
  lua_settop(L, top);

  lua_createtable(L, 4, 0);
  lua_pushnil(L);
  cursor.u32.lo = 1;
  cursor.u32.hi = LJ_KEYINDEX;
  tv_rawstore_rel(L->top - 1, tv_rawload(&cursor));
  tabroot = L->top - 2;
  keyroot = L->top - 1;
  addr = UINTPTR_MAX;
  assert(lj_tab_keyed_slot_resolve_rooted_try(L, tabroot, keyroot, &addr) ==
	 LJ_TAB_KEYED_SLOT_RETRY);
  assert(addr == 0);
  intab = tabroot;
  inkey = keyroot;
  addr = UINTPTR_MAX;
  assert(lj_tab_keyed_slot_resolve_or_insert_rooted_l(
	   L, &intab, &inkey, &addr) == LJ_TAB_KEYED_SLOT_RETRY);
  assert(addr == 0 && intab == tabroot && inkey == keyroot);
  lua_settop(L, top);

  lua_pushboolean(L, 1);  /* A stable non-table root is terminal RETRY. */
  lua_pushinteger(L, 1);
  tabroot = L->top - 2;
  keyroot = L->top - 1;
  intab = tabroot;
  inkey = keyroot;
  addr = UINTPTR_MAX;
  assert(lj_tab_keyed_slot_resolve_or_insert_rooted_l(
	   L, &intab, &inkey, &addr) == LJ_TAB_KEYED_SLOT_RETRY);
  assert(addr == 0 && intab == tabroot && inkey == keyroot);
  lua_settop(L, top);
}

static void exercise_held(lua_State *L)
{
  int top = lua_gettop(L);
  LJGC2Lease table_lease, key_lease;
  TValue tablev, keyv;
  GCtab *t;
  uintptr_t addr = UINTPTR_MAX, want;

  lua_createtable(L, 8, 0);
  lua_pushinteger(L, 3);
  lj_tv_load_acq(&tablev, L->top - 2);
  lj_tv_load_acq(&keyv, L->top - 1);
  assert(tvistab(&tablev));
  assert(lj_gc2_smr_read_try(G(L)));
  assert(lj_gc2_tv_lease_acquire(G(L), &tablev, &table_lease) ==
	 LJ_GC2_TV_EDGE_VALID);
  assert(lj_gc2_tv_lease_acquire(G(L), &keyv, &key_lease) ==
	 LJ_GC2_TV_EDGE_VALID);
  t = tabV(&tablev);
  want = ptr_addr(&lj_tab_array_acq(t)[3]);
  assert(lj_tab_keyed_slot_resolve_held(t, &keyv, &addr) ==
	 LJ_TAB_KEYED_SLOT_FOUND);
  assert(addr == want);
  lj_gc2_smr_read_leave(G(L));
  lj_gc2_lease_release(&key_lease);
  lj_gc2_lease_release(&table_lease);
  lua_settop(L, top);
}

static void exercise_forward(lua_State *L)
{
  int top = lua_gettop(L);
  GCtab *t;
  TValue saved, *slot;
  cTValue *tabroot, *keyroot;
  uintptr_t addr;

  lua_createtable(L, 8, 8);
  t = tabV(L->top - 1);
  slot = lj_tab_setint(L, t, 2);
  lj_tab_storeint(L, slot, 22);
  lua_pushinteger(L, 2);
  tabroot = L->top - 2;
  keyroot = L->top - 1;
  lj_tv_load_acq(&saved, slot);
  tabfwd_store_forward(slot);
  addr = UINTPTR_MAX;
  assert(lj_tab_keyed_slot_resolve_rooted_try(L, tabroot, keyroot, &addr) ==
	 LJ_TAB_KEYED_SLOT_RETRY);
  assert(addr == 0);
  tv_rawstore_rel(slot, tv_rawload(&saved));

  lua_pop(L, 1);
  slot = lj_tab_setint(L, t, -222);
  lj_tab_storeint(L, slot, 222);
  lua_pushinteger(L, -222);
  tabroot = L->top - 2;
  keyroot = L->top - 1;
  lj_tv_load_acq(&saved, slot);
  tabfwd_store_forward(slot);
  addr = UINTPTR_MAX;
  assert(lj_tab_keyed_slot_resolve_rooted_try(L, tabroot, keyroot, &addr) ==
	 LJ_TAB_KEYED_SLOT_RETRY);
  assert(addr == 0);
  tv_rawstore_rel(slot, tv_rawload(&saved));
  lua_settop(L, top);
}

static void exercise_retiring_vectors(lua_State *L)
{
  int top = lua_gettop(L);
  GCtab *t;
  TValue *oldarray, *newarray;
  Node *oldnode, *newnode;
  MSize oldasize, oldhmask;
  cTValue *tabroot, *keyroot;
  uintptr_t addr;

  lua_createtable(L, LJ_MAX_COLOSIZE + 32, 0);
  t = tabV(L->top - 1);
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  assert(oldarray && !lj_tab_array_is_colocated(t, oldarray));
  lj_tab_resize(L, t, (uint32_t)oldasize + 32u, 0);
  newarray = lj_tab_array_acq(t);
  assert(newarray != oldarray && lj_tab_array_is_retiring(t, oldarray));
  lj_tab_array_rel(t, oldarray);  /* Coherent root is still RETIRING. */
  lua_pushinteger(L, 3);
  tabroot = L->top - 2;
  keyroot = L->top - 1;
  addr = UINTPTR_MAX;
  assert(lj_tab_keyed_slot_resolve_rooted_try(L, tabroot, keyroot, &addr) ==
	 LJ_TAB_KEYED_SLOT_RETRY);
  assert(addr == 0);
  lj_tab_array_rel(t, newarray);
  lua_settop(L, top);

  lua_createtable(L, 0, 8);
  t = tabV(L->top - 1);
  (void)lj_tab_setint(L, t, -333);
  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  assert(oldhmask > 0);
  lj_tab_resize(L, t, lj_tab_asize_acq(t), lj_fls(oldhmask) + 2u);
  newnode = lj_tab_node_acq(t);
  assert(newnode != oldnode && lj_tab_node_is_retiring(oldnode));
  lj_tab_node_rel(t, oldnode);
  lua_pushinteger(L, -333);
  tabroot = L->top - 2;
  keyroot = L->top - 1;
  addr = UINTPTR_MAX;
  assert(lj_tab_keyed_slot_resolve_rooted_try(L, tabroot, keyroot, &addr) ==
	 LJ_TAB_KEYED_SLOT_RETRY);
  assert(addr == 0);
  lj_tab_node_rel(t, newnode);
  lua_settop(L, top);
}

static void exercise_stack_rebase(lua_State *L)
{
  int top = lua_gettop(L);
  TValue *oldstack;
  cTValue *tabroot, *keyroot;
  ptrdiff_t tabofs, keyofs;
  uintptr_t addr = UINTPTR_MAX, fresh = UINTPTR_MAX;

  lua_createtable(L, 0, 0);
  lua_pushinteger(L, -4444441);
  tabroot = L->top - 2;
  keyroot = L->top - 1;
  tabofs = savestack(L, (TValue *)(void *)tabroot);
  keyofs = savestack(L, (TValue *)(void *)keyroot);
  oldstack = tvref(L->stack);
  lj_tab_keyed_slot_test_retry_stack_grow_once();
  assert(lj_tab_keyed_slot_resolve_or_insert_rooted_l(
	   L, &tabroot, &keyroot, &addr) == LJ_TAB_KEYED_SLOT_FOUND);
  assert(lj_tab_keyed_slot_test_retry_stack_grow_hits() == 1u);
  assert(tvref(L->stack) != oldstack);
  assert(tabroot == restorestack(L, tabofs));
  assert(keyroot == restorestack(L, keyofs));
  assert(addr != 0);
  assert(lj_tab_keyed_slot_resolve_rooted_try(L, tabroot, keyroot, &fresh) ==
	 LJ_TAB_KEYED_SLOT_FOUND);
  assert(fresh == addr);
  lua_settop(L, top);
}

static void exercise_wrong_state(lua_State *L, lua_State *wrong)
{
  int top = lua_gettop(L);
  cTValue *tabroot, *keyroot;
  uintptr_t addr = UINTPTR_MAX;
  lua_createtable(L, 4, 0);
  lua_pushinteger(L, 1);
  tabroot = L->top - 2;
  keyroot = L->top - 1;
  assert(lj_tab_keyed_slot_resolve_rooted_try(
	   wrong, tabroot, keyroot, &addr) == LJ_TAB_KEYED_SLOT_RETRY);
  assert(addr == 0);
  lua_settop(L, top);
}

static void exercise_terminal_main_owner(lua_State *L)
{
  int top = lua_gettop(L);
  global_State *g = G(L);
  TGState *tg = g->main_tg;
  TGState *oldhint = L->tg_hint;
  cTValue *tabroot, *keyroot;
  uintptr_t addr = UINTPTR_MAX, fresh = UINTPTR_MAX;
  uint32_t readers0 = smr_readers(L);

  assert_rootdesc_idle(L);
  lua_createtable(L, 0, 0);
  lua_pushinteger(L, -5555551);
  tabroot = L->top - 2;
  keyroot = L->top - 1;
  assert(lj_tg_load_cur_L(tg) == L);
  mt_shutdown_rel(g, 1);
  lj_tg_clearcur_L(g);
  L->tg_hint = NULL;  /* Force the narrow terminal-owner exception. */
  assert(lj_tab_keyed_slot_resolve_or_insert_rooted_l(
	   L, &tabroot, &keyroot, &addr) == LJ_TAB_KEYED_SLOT_FOUND);
  assert(addr != 0);
  assert(lj_tab_keyed_slot_resolve_rooted_try(
	   L, tabroot, keyroot, &fresh) == LJ_TAB_KEYED_SLOT_FOUND);
  assert(fresh == addr);
  assert(smr_readers(L) == readers0);
  assert_rootdesc_idle(L);
  L->tg_hint = oldhint;
  mt_shutdown_rel(g, 0);
  lj_tg_setcur_L(g, L);
  lua_settop(L, top);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  lua_State *wrong;
  assert(L != NULL);
  luaL_openlibs(L);
  wrong = lua_newthread(L);  /* Rooted but unclaimed lua_State refusal. */
  assert(wrong != NULL);

  exercise_array_and_absent(L);
  exercise_hash_nil_and_insert(L);
  exercise_collision_insert(L);
  exercise_generic_detach_keys(L);
  exercise_invalid_keys(L);
  exercise_held(L);
  exercise_forward(L);
  exercise_retiring_vectors(L);
  exercise_stack_rebase(L);
  exercise_wrong_state(L, wrong);
  exercise_terminal_main_owner(L);

  install_close_finalizer(L);
  lua_close(L);
  assert(close_finalizer_resolver_hits == 1u);
  puts("t-tab-keyed-slot-resolver OK");
  return 0;
}
