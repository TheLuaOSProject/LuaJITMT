/*
** Focused guard for CAS-published table slot stores.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_meta.h"
#include "lj_gc2.h"
#include "lj_obj.h"
#include "lj_str.h"
#include "lj_state.h"
#include "lj_tab.h"
#include "lj_tg.h"

#include "lib/tab_forward_helpers.h"

#define WRITER_ITERS 40000

typedef struct WriterArg {
  lua_State *L;
  TValue *slot;
  int32_t base;
} WriterArg;

static void *writer_main(void *arg)
{
  WriterArg *w = (WriterArg *)arg;
  int32_t i;
  for (i = 0; i < WRITER_ITERS; i++) {
    TValue src;
    setintV(&src, w->base + i);
    assert(lj_tab_trystoretv_cas(w->L, w->slot, &src) ==
	   LJ_TAB_STORE_CAS_OK);
  }
  return NULL;
}

static void exercise_direct_cas(lua_State *L)
{
  TValue slot, src, val;
  pthread_t a, b;
  WriterArg wa, wb;

  setnilV(&slot);
  setintV(&src, 42);
  assert(lj_tab_trystoretv_cas(L, &slot, &src) == LJ_TAB_STORE_CAS_OK);
  lj_tv_load_acq(&val, &slot);
  tabfwd_assert_i32(&val, 42);

  wa.L = L; wa.slot = &slot; wa.base = 100000;
  wb.L = L; wb.slot = &slot; wb.base = 200000;
  assert(pthread_create(&a, NULL, writer_main, &wa) == 0);
  assert(pthread_create(&b, NULL, writer_main, &wb) == 0);
  assert(pthread_join(a, NULL) == 0);
  assert(pthread_join(b, NULL) == 0);
  lj_tv_load_acq(&val, &slot);
  assert(tvisnumber(&val));
  assert(!tvisforward(&val));

  tabfwd_store_forward(&slot);
  setintV(&src, 99);
  assert(lj_tab_trystoretv_cas(L, &slot, &src) ==
	 LJ_TAB_STORE_CAS_FORWARD);
  tabfwd_assert_forward(&slot);
}

static void exercise_meta_forward_retry(lua_State *L)
{
  GCtab *t;
  TValue *oldarray, *newarray, key, val;
  MSize oldasize, newasize, oldacap;
  int32_t k = 5;
  MSize i;

  lua_settop(L, 0);
  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  oldacap = t->acap;
  assert((MSize)k < oldasize);
  for (i = 0; i < oldasize; i++)
    lj_tab_storeint(L, lj_tab_setint(L, t, (int32_t)i), (int32_t)i + 1000);

  lj_tab_resize(L, t, (uint32_t)oldasize + 8u, 0);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  assert(newarray != oldarray);
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);
  tabfwd_assert_i32(lj_tab_getint(t, k), k + 1000);

  tabfwd_store_forward(&oldarray[k]);
  la_store32_rel(&lj_tab_array_hdrw(oldarray)->acap,
		 lj_tab_array_hdr_pack_acap(oldacap, 0));
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);

  settabV(L, &L->top[0], t);
  setintV(&key, k);
  setintV(&val, 4242);
  assert(lj_meta_tsettv_pair(L, &L->top[0], &key, &val) == &newarray[k]);
  tabfwd_assert_forward(&oldarray[k]);
  tabfwd_assert_i32(&newarray[k], 4242);
  tabfwd_assert_i32(lj_tab_getint(t, k), 4242);

  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
  lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);
}

static void exercise_capi_rawseti_forward_retry(lua_State *L)
{
  GCtab *t;
  TValue *oldarray, *newarray;
  MSize oldasize, newasize, oldacap;
  int32_t k = 6;
  MSize i;

  lua_settop(L, 0);
  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  oldacap = t->acap;
  assert((MSize)k < oldasize);
  for (i = 0; i < oldasize; i++)
    lj_tab_storeint(L, lj_tab_setint(L, t, (int32_t)i), (int32_t)i + 2000);

  lj_tab_resize(L, t, (uint32_t)oldasize + 8u, 0);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  assert(newarray != oldarray);
  tabfwd_store_forward(&oldarray[k]);
  la_store32_rel(&lj_tab_array_hdrw(oldarray)->acap,
		 lj_tab_array_hdr_pack_acap(oldacap, 0));
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);

  lua_pushinteger(L, 5252);
  lua_rawseti(L, -2, k);
  tabfwd_assert_forward(&oldarray[k]);
  tabfwd_assert_i32(&newarray[k], 5252);

  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
  lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);
}

static void exercise_capi_settable_forward_retry(lua_State *L)
{
  GCtab *t;
  TValue *oldarray, *newarray;
  MSize oldasize, newasize, oldacap;
  int32_t k = 7;
  MSize i;

  lua_settop(L, 0);
  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  oldacap = t->acap;
  assert((MSize)k < oldasize);
  for (i = 0; i < oldasize; i++)
    lj_tab_storeint(L, lj_tab_setint(L, t, (int32_t)i), (int32_t)i + 3000);

  lj_tab_resize(L, t, (uint32_t)oldasize + 8u, 0);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  assert(newarray != oldarray);
  tabfwd_store_forward(&oldarray[k]);
  la_store32_rel(&lj_tab_array_hdrw(oldarray)->acap,
		 lj_tab_array_hdr_pack_acap(oldacap, 0));
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);

  lua_pushinteger(L, k);
  lua_pushinteger(L, 6262);
  lua_settable(L, -3);
  tabfwd_assert_forward(&oldarray[k]);
  tabfwd_assert_i32(&newarray[k], 6262);

  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
  lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);
}

static void exercise_capi_rawset_forward_retry(lua_State *L)
{
  GCtab *t;
  GCstr *key;
  Node *oldnode, *newnode, *oldn, *newn;
  MSize oldhmask, newhmask;

  lua_settop(L, 0);
  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  key = lj_str_newlit(L, "capi_rawset_forward");
  lj_tab_storeint(L, lj_tab_setstr(L, t, key), 7000);
  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  oldn = tabfwd_find_str_node(oldnode, oldhmask, key);
  assert(oldn != NULL);

  lj_tab_resize(L, t, t->asize, lj_fls(oldhmask) + 2u);
  newnode = lj_tab_node_acq(t);
  newhmask = lj_tab_node_hmask_acq(newnode);
  assert(newnode != oldnode);
  newn = tabfwd_find_str_node(newnode, newhmask, key);
  assert(newn != NULL);
  tabfwd_store_forward(&oldn->val);
  la_store32_rel(&lj_tab_node_hdrw(oldnode)->flags, 0);
  lj_tab_hmask_rel(t, oldhmask);
  lj_tab_node_rel(t, oldnode);

  setstrV(L, L->top, key);
  incr_top(L);
  lua_pushinteger(L, 7272);
  lua_rawset(L, -3);
  tabfwd_assert_forward(&oldn->val);
  tabfwd_assert_i32(&newn->val, 7272);

  lj_tab_node_rel(t, newnode);
  lj_tab_hmask_rel(t, newhmask);
  lj_tab_node_hdr_flags_or_rel(oldnode, TABNODE_FLAG_RETIRING);
}

static void exercise_capi_setfield_forward_retry(lua_State *L)
{
  GCtab *t;
  const char *name = "capi_setfield_forward";
  GCstr *key;
  Node *oldnode, *newnode, *oldn, *newn;
  MSize oldhmask, newhmask;

  lua_settop(L, 0);
  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  key = lj_str_newz(L, name);
  lj_tab_storeint(L, lj_tab_setstr(L, t, key), 8000);
  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  oldn = tabfwd_find_str_node(oldnode, oldhmask, key);
  assert(oldn != NULL);

  lj_tab_resize(L, t, t->asize, lj_fls(oldhmask) + 2u);
  newnode = lj_tab_node_acq(t);
  newhmask = lj_tab_node_hmask_acq(newnode);
  assert(newnode != oldnode);
  newn = tabfwd_find_str_node(newnode, newhmask, key);
  assert(newn != NULL);
  tabfwd_store_forward(&oldn->val);
  la_store32_rel(&lj_tab_node_hdrw(oldnode)->flags, 0);
  lj_tab_hmask_rel(t, oldhmask);
  lj_tab_node_rel(t, oldnode);

  lua_pushinteger(L, 8282);
  lua_setfield(L, -2, name);
  tabfwd_assert_forward(&oldn->val);
  tabfwd_assert_i32(&newn->val, 8282);

  lj_tab_node_rel(t, newnode);
  lj_tab_hmask_rel(t, newhmask);
  lj_tab_node_hdr_flags_or_rel(oldnode, TABNODE_FLAG_RETIRING);
}

static void call_table_insert(lua_State *L, int32_t pos, int32_t val)
{
  lua_getglobal(L, "table");
  lua_getfield(L, -1, "insert");
  lua_remove(L, -2);
  lua_pushvalue(L, -2);
  lua_pushinteger(L, pos);
  lua_pushinteger(L, val);
  lua_call(L, 3, 0);
}

static void exercise_table_insert_forward_retry(lua_State *L)
{
  GCtab *t;
  TValue *oldarray, *newarray;
  MSize oldasize, newasize, oldacap;
  int32_t pos = 3;
  MSize i;

  lua_settop(L, 0);
  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  oldacap = t->acap;
  assert(oldasize > 6);
  for (i = 1; i <= 5; i++)
    lj_tab_storeint(L, lj_tab_setint(L, t, (int32_t)i), (int32_t)i + 4000);

  lj_tab_resize(L, t, (uint32_t)oldasize + 8u, 0);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  assert(newarray != oldarray);
  for (i = (MSize)pos; i <= 6; i++)
    tabfwd_store_forward(&oldarray[i]);
  la_store32_rel(&lj_tab_array_hdrw(oldarray)->acap,
		 lj_tab_array_hdr_pack_acap(oldacap, 0));
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);

  call_table_insert(L, pos, 9090);
  for (i = (MSize)pos; i <= 6; i++)
    tabfwd_assert_forward(&oldarray[i]);
  tabfwd_assert_i32(&newarray[3], 9090);
  tabfwd_assert_i32(&newarray[4], 4003);
  tabfwd_assert_i32(&newarray[5], 4004);
  tabfwd_assert_i32(&newarray[6], 4005);

  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
  lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);
}

static void exercise_tsetm_helper_forward_retry(lua_State *L)
{
  GCtab *t;
  TValue *oldarray, *newarray;
  TValue src[3];
  MSize oldasize, newasize, oldacap;
  int32_t start = 4;
  MSize i;

  lua_settop(L, 0);
  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  oldacap = t->acap;
  assert((MSize)(start + 2) < oldasize);
  for (i = 0; i < oldasize; i++)
    lj_tab_storeint(L, lj_tab_setint(L, t, (int32_t)i), (int32_t)i + 5000);

  lj_tab_resize(L, t, (uint32_t)oldasize + 8u, 0);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  assert(newarray != oldarray);
  for (i = 0; i < 3; i++)
    tabfwd_store_forward(&oldarray[start + i]);
  la_store32_rel(&lj_tab_array_hdrw(oldarray)->acap,
		 lj_tab_array_hdr_pack_acap(oldacap, 0));
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);

  setintV(&src[0], 6161);
  setintV(&src[1], 6262);
  setintV(&src[2], 6363);
  lj_tab_storetvn_forvm_array(L, t, (uint32_t)start, src, 3);
  for (i = 0; i < 3; i++)
    tabfwd_assert_forward(&oldarray[start + i]);
  tabfwd_assert_i32(&newarray[start], 6161);
  tabfwd_assert_i32(&newarray[start + 1], 6262);
  tabfwd_assert_i32(&newarray[start + 2], 6363);

  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
  lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);
}

static void exercise_tsetm_helper_current_retiring(lua_State *L)
{
  GCtab *t;
  TValue *oldarray, *newarray;
  TValue src[3], oldval;
  MSize oldasize, newasize;
  int32_t start = 5;
  MSize i;

  lua_settop(L, 0);
  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  assert((MSize)(start + 2) < oldasize);
  for (i = 0; i < oldasize; i++)
    lj_tab_storeint(L, lj_tab_setint(L, t, (int32_t)i), (int32_t)i + 7000);

  lj_tab_resize(L, t, (uint32_t)oldasize + 8u, 0);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  assert(newarray != oldarray);
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);
  assert(lj_tab_array_is_retiring(t, oldarray));

  lj_tab_array_rel(t, oldarray);
  lj_tab_asize_rel(t, oldasize);

  setintV(&src[0], 8181);
  setintV(&src[1], 8282);
  setintV(&src[2], 8383);
  lj_tab_storetvn_forvm_array(L, t, (uint32_t)start, src, 3);
  for (i = 0; i < 3; i++) {
    lj_tv_load_acq(&oldval, &oldarray[start + i]);
    tabfwd_assert_i32(&oldval, start + (int32_t)i + 7000);
  }
  tabfwd_assert_i32(&newarray[start], 8181);
  tabfwd_assert_i32(&newarray[start + 1], 8282);
  tabfwd_assert_i32(&newarray[start + 2], 8383);

  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
}

static void exercise_tsetm_helper_post_barrier(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  TValue src[2];
  GCtab *t;
  uint32_t old_mark_active, old_phase, old_generational, old_minor_sweep;
  uint64_t remembered0;

  assert(tg != NULL);
  old_mark_active = la_load32_acq(&tg->mark_active);
  old_phase = la_load32_acq(&g->gc2.phase);
  old_generational = la_load32_acq(&g->gc2.generational);
  old_minor_sweep = la_load32_acq(&g->gc2.minor_sweep_enabled);

  lua_settop(L, 0);
  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  setintV(&src[0], 7171);
  setintV(&src[1], 7272);

  la_store32_rel(&g->gc2.phase, LJ_GC2_IDLE);
  la_store32_rel(&g->gc2.generational, 1);
  la_store32_rel(&g->gc2.minor_sweep_enabled, 1);
  la_store32_rel(&tg->mark_active, 1);
  remembered0 = la_load64_acq(&g->gc2.remembered_barriers);
  lj_tab_storetvn_forvm_array(L, t, 4, src, 2);
  assert(la_load64_acq(&g->gc2.remembered_barriers) > remembered0);
  tabfwd_assert_i32(lj_tab_getint(t, 4), 7171);
  tabfwd_assert_i32(lj_tab_getint(t, 5), 7272);

  la_store32_rel(&tg->mark_active, old_mark_active);
  la_store32_rel(&g->gc2.phase, old_phase);
  la_store32_rel(&g->gc2.generational, old_generational);
  la_store32_rel(&g->gc2.minor_sweep_enabled, old_minor_sweep);
}

static void exercise_luaL_newmetatable_forward_retry(lua_State *L)
{
  GCtab *reg = tabV(registry(L));
  const char *name = "capi_newmetatable_forward";
  GCstr *key = lj_str_newz(L, name);
  Node *oldnode, *newnode, *oldn, *newn;
  MSize oldhmask, newhmask;
  TValue nv;

  lua_settop(L, 0);
  lj_tab_storeint(L, lj_tab_setstr(L, reg, key), 9100);
  oldnode = lj_tab_node_acq(reg);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  oldn = tabfwd_find_str_node(oldnode, oldhmask, key);
  assert(oldn != NULL);

  lj_tab_resize(L, reg, reg->asize, lj_fls(oldhmask) + 2u);
  newnode = lj_tab_node_acq(reg);
  newhmask = lj_tab_node_hmask_acq(newnode);
  assert(newnode != oldnode);
  newn = tabfwd_find_str_node(newnode, newhmask, key);
  assert(newn != NULL);
  lj_tab_storenilraw(&newn->val);
  tabfwd_store_forward(&oldn->val);
  la_store32_rel(&lj_tab_node_hdrw(oldnode)->flags, 0);
  lj_tab_hmask_rel(reg, oldhmask);
  lj_tab_node_rel(reg, oldnode);

  assert(luaL_newmetatable(L, name) == 1);
  tabfwd_assert_forward(&oldn->val);
  lj_tv_load_acq(&nv, &newn->val);
  assert(tvistab(&nv));
  assert(tabV(&nv) == tabV(L->top-1));

  lj_tab_node_rel(reg, newnode);
  lj_tab_hmask_rel(reg, newhmask);
  lj_tab_node_hdr_flags_or_rel(oldnode, TABNODE_FLAG_RETIRING);
  lua_pop(L, 1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);

  exercise_direct_cas(L);
  exercise_meta_forward_retry(L);
  exercise_capi_rawseti_forward_retry(L);
  exercise_capi_settable_forward_retry(L);
  exercise_capi_rawset_forward_retry(L);
  exercise_capi_setfield_forward_retry(L);
  exercise_table_insert_forward_retry(L);
  exercise_tsetm_helper_forward_retry(L);
  exercise_tsetm_helper_current_retiring(L);
  exercise_tsetm_helper_post_barrier(L);
  exercise_luaL_newmetatable_forward_retry(L);

  lua_close(L);
  printf("t-tab-cas-store OK: CAS table stores preserve FORWARD slots\n");
  return 0;
}
