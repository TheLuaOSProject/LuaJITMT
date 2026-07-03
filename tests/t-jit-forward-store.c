/*
** Focused regression test for helper-backed JIT stores over forwarded table slots.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_str.h"
#include "lj_tab.h"

#include "lib/tab_forward_helpers.h"

#ifdef LJ_TAB_TEST_HELPERS
LJ_FUNCA TValue *lj_tab_test_storetv_forjit_array_observed(lua_State *L,
							   GCtab *parent,
							   TValue *array,
							   MSize asize,
							   TValue *dst,
							   cTValue *src,
							   MSize key);

LJ_FUNCA TValue *lj_tab_test_storetv_forjit_hash_observed(lua_State *L,
							  GCtab *parent,
							  Node *node,
							  MSize hmask,
							  TValue *dst,
							  cTValue *src);
#endif

static void exercise_array_forward_jit(lua_State *L)
{
  GCtab *t;
  TValue *oldarray, *newarray;
  TValue src;
  MSize oldasize, newasize, oldacap;
  int32_t key = 3;
  MSize i;

  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  oldacap = t->acap;
  assert((MSize)key < oldasize);
  for (i = 0; i < oldasize; i++) {
    int32_t v = (int32_t)i + 12000;
    tabfwd_set_int(L, t, (int32_t)i, v);
    lj_tab_storeint(L, &oldarray[i], v);
  }

  lj_tab_resize(L, t, (uint32_t)oldasize + 8u, 0);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  assert(newarray != oldarray);
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);

  tabfwd_store_forward(&oldarray[key]);
  la_store32_rel(&lj_tab_array_hdrw(oldarray)->acap,
		 lj_tab_array_hdr_pack_acap(oldacap, 0));
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);

  setintV(&src, 200);
  lj_tab_storetv_forjit_array(L, t, &oldarray[key], &src, (MSize)key);
  tabfwd_assert_forward(&oldarray[key]);
  tabfwd_assert_i32(&newarray[key], 200);

  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
  lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);
  lua_pop(L, 1);
}

static void exercise_array_retiring_jit(lua_State *L)
{
  GCtab *t;
  TValue *oldarray, *newarray;
  TValue src, oldval;
  MSize oldasize, newasize;
  int32_t key = 6;

  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  assert((MSize)key < oldasize);
  tabfwd_set_int(L, t, key, 16000);
  tabfwd_assert_i32(&oldarray[key], 16000);

  lj_tab_resize(L, t, (uint32_t)oldasize + 8u, 0);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  assert(newarray != oldarray);
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);
  assert(lj_tab_array_is_retiring(t, oldarray));

  setintV(&src, 201);
  lj_tab_storetv_forjit_array(L, t, &oldarray[key], &src, (MSize)key);
  lj_tv_load_acq(&oldval, &oldarray[key]);
  assert(tvisforward(&oldval));
  tabfwd_assert_i32(&newarray[key], 201);
  tabfwd_assert_i32(lj_tab_getint(t, key), 201);

  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
  lua_pop(L, 1);
}

static void exercise_array_current_retiring_jit(lua_State *L)
{
  GCtab *t;
  TValue *oldarray, *newarray;
  TValue src, oldval;
  MSize oldasize, newasize;
  int32_t key = 7;

  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  assert((MSize)key < oldasize);
  tabfwd_set_int(L, t, key, 18000);
  tabfwd_assert_i32(&oldarray[key], 18000);

  lj_tab_resize(L, t, (uint32_t)oldasize + 8u, 0);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  assert(newarray != oldarray);
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);
  assert(lj_tab_array_is_retiring(t, oldarray));

  lj_tab_array_rel(t, oldarray);
  lj_tab_asize_rel(t, oldasize);
  setintV(&src, 204);
  lj_tab_storetv_forjit_array(L, t, &oldarray[key], &src, (MSize)key);
  lj_tv_load_acq(&oldval, &oldarray[key]);
  assert(tvisforward(&oldval));

  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
  tabfwd_assert_i32(&newarray[key], 204);
  tabfwd_assert_i32(lj_tab_getint(t, key), 204);

  lua_pop(L, 1);
}

#ifdef LJ_TAB_TEST_HELPERS
static void exercise_array_retiring_observed_jit(lua_State *L)
{
  GCtab *t;
  TValue *oldarray, *newarray;
  TValue src, oldval;
  TValue *slot;
  MSize oldasize, newasize;
  int32_t key = 8;

  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  assert((MSize)key < oldasize);
  tabfwd_set_int(L, t, key, 18100);
  tabfwd_assert_i32(&oldarray[key], 18100);

  lj_tab_resize(L, t, (uint32_t)oldasize + 8u, 0);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  assert(newarray != oldarray);
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);
  assert(lj_tab_array_is_retiring(t, oldarray));

  setintV(&src, 207);
  slot = lj_tab_test_storetv_forjit_array_observed(L, t, oldarray,
						   oldasize, &oldarray[key],
						   &src, (MSize)key);
  assert(slot == &newarray[key]);
  lj_tv_load_acq(&oldval, &oldarray[key]);
  assert(tvisforward(&oldval));
  tabfwd_assert_i32(&newarray[key], 207);
  tabfwd_assert_i32(lj_tab_getint(t, key), 207);

  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
  lua_pop(L, 1);
}
#endif

static void exercise_hash_forward_jit(lua_State *L)
{
  GCtab *t;
  GCstr *hkey;
  TValue src, keytv;
  Node *oldnode, *newnode, *oldn, *newn;
  MSize oldhmask, newhmask;

  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  hkey = lj_str_new(L, "jit_forward_hash", strlen("jit_forward_hash"));
  lj_tab_storeint(L, lj_tab_setstr(L, t, hkey), 13000);
  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  assert(oldhmask > 0);
  oldn = tabfwd_find_str_node(oldnode, oldhmask, hkey);
  assert(oldn != NULL);

  lj_tab_resize(L, t, t->asize, lj_fls(oldhmask) + 2u);
  newnode = lj_tab_node_acq(t);
  newhmask = lj_tab_node_hmask_acq(newnode);
  assert(newnode != oldnode);
  assert(lj_tab_node_nextgen_acq(oldnode) == newnode);
  newn = tabfwd_find_str_node(newnode, newhmask, hkey);
  assert(newn != NULL);

  tabfwd_store_forward(&oldn->val);
  la_store32_rel(&lj_tab_node_hdrw(oldnode)->flags, 0);
  lj_tab_hmask_rel(t, oldhmask);
  lj_tab_node_rel(t, oldnode);

  setintV(&src, 200);
  setstrV(L, &keytv, hkey);
  lj_tab_storetv_forjit_hash(L, t, &oldn->val, &src, &keytv);
  tabfwd_assert_forward(&oldn->val);
  tabfwd_assert_i32(&newn->val, 200);

  lj_tab_node_rel(t, newnode);
  lj_tab_hmask_rel(t, newhmask);
  lj_tab_node_hdr_flags_or_rel(oldnode, TABNODE_FLAG_RETIRING);
  lua_pop(L, 1);
}

#ifdef LJ_TAB_TEST_HELPERS
static void exercise_hash_forward_observed_jit(lua_State *L)
{
  GCtab *t;
  GCstr *hkey;
  TValue src;
  TValue *slot;
  Node *oldnode, *newnode, *oldn, *newn;
  MSize oldhmask, newhmask;

  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  hkey = lj_str_new(L, "jit_forward_observed_hash",
		    strlen("jit_forward_observed_hash"));
  lj_tab_storeint(L, lj_tab_setstr(L, t, hkey), 13100);
  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  assert(oldhmask > 0);
  oldn = tabfwd_find_str_node(oldnode, oldhmask, hkey);
  assert(oldn != NULL);

  lj_tab_resize(L, t, t->asize, lj_fls(oldhmask) + 2u);
  newnode = lj_tab_node_acq(t);
  newhmask = lj_tab_node_hmask_acq(newnode);
  assert(newnode != oldnode);
  assert(lj_tab_node_nextgen_acq(oldnode) == newnode);
  assert(lj_tab_node_is_retiring(oldnode));
  newn = tabfwd_find_str_node(newnode, newhmask, hkey);
  assert(newn != NULL);

  tabfwd_store_forward(&oldn->val);
  setintV(&src, 206);
  slot = lj_tab_test_storetv_forjit_hash_observed(L, t, oldnode, oldhmask,
						  &oldn->val, &src);
  assert(slot == &newn->val);
  tabfwd_assert_forward(&oldn->val);
  tabfwd_assert_i32(&newn->val, 206);
  tabfwd_assert_i32(lj_tab_getstr(t, hkey), 206);

  lua_pop(L, 1);
}

static void exercise_hash_retiring_observed_jit(lua_State *L)
{
  GCtab *t;
  GCstr *hkey;
  TValue src, oldval;
  TValue *slot;
  Node *oldnode, *newnode, *oldn, *newn;
  MSize oldhmask, newhmask;

  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  hkey = lj_str_new(L, "jit_retiring_observed_hash",
		    strlen("jit_retiring_observed_hash"));
  lj_tab_storeint(L, lj_tab_setstr(L, t, hkey), 19100);
  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  assert(oldhmask > 0);
  oldn = tabfwd_find_str_node(oldnode, oldhmask, hkey);
  assert(oldn != NULL);

  lj_tab_resize(L, t, t->asize, lj_fls(oldhmask) + 2u);
  newnode = lj_tab_node_acq(t);
  newhmask = lj_tab_node_hmask_acq(newnode);
  assert(newnode != oldnode);
  assert(lj_tab_node_nextgen_acq(oldnode) == newnode);
  assert(lj_tab_node_is_retiring(oldnode));
  newn = tabfwd_find_str_node(newnode, newhmask, hkey);
  assert(newn != NULL);

  setintV(&src, 208);
  slot = lj_tab_test_storetv_forjit_hash_observed(L, t, oldnode, oldhmask,
						  &oldn->val, &src);
  assert(slot == &newn->val);
  lj_tv_load_acq(&oldval, &oldn->val);
  assert(tvisforward(&oldval));
  tabfwd_assert_i32(&newn->val, 208);
  tabfwd_assert_i32(lj_tab_getstr(t, hkey), 208);

  lua_pop(L, 1);
}
#endif

static void exercise_hash_retiring_jit(lua_State *L)
{
  GCtab *t;
  GCstr *hkey;
  TValue src, keytv, oldval;
  Node *oldnode, *oldn;
  MSize oldhmask;

  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  hkey = lj_str_new(L, "jit_retiring_hash", strlen("jit_retiring_hash"));
  lj_tab_storeint(L, lj_tab_setstr(L, t, hkey), 17000);
  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  assert(oldhmask > 0);
  oldn = tabfwd_find_str_node(oldnode, oldhmask, hkey);
  assert(oldn != NULL);

  lj_tab_resize(L, t, t->asize, lj_fls(oldhmask) + 2u);
  assert(lj_tab_node_acq(t) != oldnode);
  assert(lj_tab_node_nextgen_acq(oldnode) == lj_tab_node_acq(t));
  assert(lj_tab_node_is_retiring(oldnode));

  setintV(&src, 202);
  setstrV(L, &keytv, hkey);
  lj_tab_storetv_forjit_hash(L, t, &oldn->val, &src, &keytv);
  lj_tv_load_acq(&oldval, &oldn->val);
  assert(tvisforward(&oldval));
  tabfwd_assert_i32(lj_tab_getstr(t, hkey), 202);

  lua_pop(L, 1);
}

static void exercise_hash_current_retiring_jit(lua_State *L)
{
  GCtab *t;
  GCstr *hkey;
  TValue src, keytv, oldval;
  Node *oldnode, *newnode, *oldn, *newn;
  MSize oldhmask, newhmask;

  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  hkey = lj_str_new(L, "jit_current_retiring_hash",
		    strlen("jit_current_retiring_hash"));
  lj_tab_storeint(L, lj_tab_setstr(L, t, hkey), 19000);
  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  assert(oldhmask > 0);
  oldn = tabfwd_find_str_node(oldnode, oldhmask, hkey);
  assert(oldn != NULL);

  lj_tab_resize(L, t, t->asize, lj_fls(oldhmask) + 2u);
  newnode = lj_tab_node_acq(t);
  newhmask = lj_tab_node_hmask_acq(newnode);
  assert(newnode != oldnode);
  assert(lj_tab_node_nextgen_acq(oldnode) == newnode);
  assert(lj_tab_node_is_retiring(oldnode));
  newn = tabfwd_find_str_node(newnode, newhmask, hkey);
  assert(newn != NULL);

  lj_tab_node_rel(t, oldnode);
  lj_tab_hmask_rel(t, oldhmask);
  setintV(&src, 205);
  setstrV(L, &keytv, hkey);
  lj_tab_storetv_forjit_hash(L, t, &oldn->val, &src, &keytv);
  lj_tv_load_acq(&oldval, &oldn->val);
  assert(tvisforward(&oldval));

  lj_tab_node_rel(t, newnode);
  lj_tab_hmask_rel(t, newhmask);
  tabfwd_assert_i32(&newn->val, 205);
  tabfwd_assert_i32(lj_tab_getstr(t, hkey), 205);

  lua_pop(L, 1);
}

static void exercise_newref_array_forward_jit(lua_State *L)
{
  GCtab *t;
  TValue *oldarray, *newarray;
  TValue src, keytv;
  MSize oldasize, newasize;
  int32_t key = 4;
  MSize i;

  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  assert((MSize)key < oldasize);
  for (i = 0; i < oldasize; i++)
    tabfwd_set_int(L, t, (int32_t)i, (int32_t)i + 14000);

  lj_tab_resize(L, t, (uint32_t)oldasize + 8u, 0);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  assert(newarray != oldarray);
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);

  tabfwd_store_forward(&oldarray[key]);
  setintV(&src, 300);
  setnumV(&keytv, (lua_Number)key);
  lj_tab_storetv_forjit_newref(L, t, &oldarray[key], &src, &keytv);
  tabfwd_assert_forward(&oldarray[key]);
  tabfwd_assert_i32(&newarray[key], 300);
  tabfwd_assert_i32(lj_tab_getint(t, key), 300);

  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
  lua_pop(L, 1);
}

static void exercise_newref_array_retiring_jit(lua_State *L)
{
  GCtab *t;
  TValue *oldarray, *newarray;
  TValue src, keytv, oldval;
  MSize oldasize, newasize;
  int32_t key = 5;

  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  assert((MSize)key < oldasize);
  lj_tv_load_acq(&oldval, &oldarray[key]);
  assert(tvisnil(&oldval));

  lj_tab_resize(L, t, (uint32_t)oldasize + 8u, 0);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  assert(newarray != oldarray);
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);
  assert(lj_tab_array_is_retiring(t, oldarray));

  setintV(&src, 302);
  setnumV(&keytv, (lua_Number)key);
  lj_tab_storetv_forjit_newref(L, t, &oldarray[key], &src, &keytv);
  lj_tv_load_acq(&oldval, &oldarray[key]);
  assert(tvisnil(&oldval));
  tabfwd_assert_i32(&newarray[key], 302);
  tabfwd_assert_i32(lj_tab_getint(t, key), 302);

  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
  lua_pop(L, 1);
}

static void exercise_newref_hash_forward_jit(lua_State *L)
{
  GCtab *t;
  TValue src, keytv;
  Node *oldnode, *newnode, *oldn, *newn;
  MSize oldhmask, newhmask;

  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  setnumV(&keytv, (lua_Number)1000000);
  lj_tab_storeint(L, lj_tab_set(L, t, &keytv), 15000);
  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  assert(oldhmask > 0);
  oldn = tabfwd_find_num_node(oldnode, oldhmask, &keytv);
  assert(oldn != NULL);

  lj_tab_resize(L, t, t->asize, lj_fls(oldhmask) + 2u);
  newnode = lj_tab_node_acq(t);
  newhmask = lj_tab_node_hmask_acq(newnode);
  assert(newnode != oldnode);
  assert(lj_tab_node_nextgen_acq(oldnode) == newnode);
  newn = tabfwd_find_num_node(newnode, newhmask, &keytv);
  assert(newn != NULL);

  tabfwd_store_forward(&oldn->val);
  setintV(&src, 301);
  lj_tab_storetv_forjit_newref(L, t, &oldn->val, &src, &keytv);
  tabfwd_assert_forward(&oldn->val);
  tabfwd_assert_i32(&newn->val, 301);
  tabfwd_assert_i32(lj_tab_get(L, t, &keytv), 301);

  lj_tab_node_rel(t, newnode);
  lj_tab_hmask_rel(t, newhmask);
  lua_pop(L, 1);
}

static void exercise_newref_hash_retiring_jit(lua_State *L)
{
  GCtab *t;
  TValue src, keytv, oldval;
  Node *oldnode, *oldn;
  MSize oldhmask;

  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  setnumV(&keytv, (lua_Number)1000001);
  (void)lj_tab_set(L, t, &keytv);
  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  assert(oldhmask > 0);
  oldn = tabfwd_find_num_node(oldnode, oldhmask, &keytv);
  assert(oldn != NULL);
  lj_tv_load_acq(&oldval, &oldn->val);
  assert(tvisnil(&oldval));

  lj_tab_resize(L, t, t->asize, lj_fls(oldhmask) + 2u);
  assert(lj_tab_node_acq(t) != oldnode);
  assert(lj_tab_node_nextgen_acq(oldnode) == lj_tab_node_acq(t));
  assert(lj_tab_node_is_retiring(oldnode));

  setintV(&src, 303);
  lj_tab_storetv_forjit_newref(L, t, &oldn->val, &src, &keytv);
  lj_tv_load_acq(&oldval, &oldn->val);
  assert(tvisnil(&oldval));
  tabfwd_assert_i32(lj_tab_get(L, t, &keytv), 303);

  lua_pop(L, 1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);

  exercise_array_forward_jit(L);
  exercise_array_retiring_jit(L);
  exercise_array_current_retiring_jit(L);
#ifdef LJ_TAB_TEST_HELPERS
  exercise_array_retiring_observed_jit(L);
#endif
  exercise_hash_forward_jit(L);
#ifdef LJ_TAB_TEST_HELPERS
  exercise_hash_forward_observed_jit(L);
  exercise_hash_retiring_observed_jit(L);
#endif
  exercise_hash_retiring_jit(L);
  exercise_hash_current_retiring_jit(L);
  exercise_newref_array_forward_jit(L);
  exercise_newref_array_retiring_jit(L);
  exercise_newref_hash_forward_jit(L);
  exercise_newref_hash_retiring_jit(L);

  lua_close(L);
  printf("t-jit-forward-store OK: helper-backed JIT stores route forwarded slots\n");
  return 0;
}
