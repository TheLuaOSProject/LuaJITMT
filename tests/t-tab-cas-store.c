/*
** Focused guard for CAS-published table slot stores.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_meta.h"
#include "lj_obj.h"
#include "lj_str.h"
#include "lj_state.h"
#include "lj_tab.h"

#define WRITER_ITERS 40000

typedef struct WriterArg {
  lua_State *L;
  TValue *slot;
  int32_t base;
} WriterArg;

static void store_forward(TValue *slot)
{
  TValue forward;
  setforwardV(&forward);
  tv_rawstore_rel(slot, tv_rawload(&forward));
}

static int32_t tv_i32(cTValue *tv)
{
  assert(tv != NULL);
  assert(tvisnumber(tv));
  return tvisint(tv) ? intV(tv) : (int32_t)numV(tv);
}

static void assert_i32(cTValue *tv, int32_t want)
{
  assert(tv_i32(tv) == want);
}

static void assert_forward(cTValue *tv)
{
  TValue val;
  lj_tv_load_acq(&val, tv);
  assert(tvisforward(&val));
}

static Node *find_str_node(Node *node, MSize hmask, const GCstr *key)
{
  Node *n = hashstr_node(node, hmask, key);
  do {
    TValue nk;
    lj_tv_load_acq(&nk, &n->key);
    if (tvisstr(&nk) && strV(&nk) == key)
      return n;
  } while ((n = lj_tab_nextnode_acq(n)));
  return NULL;
}

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
  assert_i32(&val, 42);

  wa.L = L; wa.slot = &slot; wa.base = 100000;
  wb.L = L; wb.slot = &slot; wb.base = 200000;
  assert(pthread_create(&a, NULL, writer_main, &wa) == 0);
  assert(pthread_create(&b, NULL, writer_main, &wb) == 0);
  assert(pthread_join(a, NULL) == 0);
  assert(pthread_join(b, NULL) == 0);
  lj_tv_load_acq(&val, &slot);
  assert(tvisnumber(&val));
  assert(!tvisforward(&val));

  store_forward(&slot);
  setintV(&src, 99);
  assert(lj_tab_trystoretv_cas(L, &slot, &src) ==
	 LJ_TAB_STORE_CAS_FORWARD);
  assert_forward(&slot);
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
  assert_i32(lj_tab_getint(t, k), k + 1000);

  store_forward(&oldarray[k]);
  la_store32_rel(&lj_tab_array_hdrw(oldarray)->acap,
		 lj_tab_array_hdr_pack_acap(oldacap, 0));
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);

  settabV(L, &L->top[0], t);
  setintV(&key, k);
  setintV(&val, 4242);
  assert(lj_meta_tsettv_pair(L, &L->top[0], &key, &val) == &newarray[k]);
  assert_forward(&oldarray[k]);
  assert_i32(&newarray[k], 4242);
  assert_i32(lj_tab_getint(t, k), 4242);

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
  store_forward(&oldarray[k]);
  la_store32_rel(&lj_tab_array_hdrw(oldarray)->acap,
		 lj_tab_array_hdr_pack_acap(oldacap, 0));
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);

  lua_pushinteger(L, 5252);
  lua_rawseti(L, -2, k);
  assert_forward(&oldarray[k]);
  assert_i32(&newarray[k], 5252);

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
  store_forward(&oldarray[k]);
  la_store32_rel(&lj_tab_array_hdrw(oldarray)->acap,
		 lj_tab_array_hdr_pack_acap(oldacap, 0));
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);

  lua_pushinteger(L, k);
  lua_pushinteger(L, 6262);
  lua_settable(L, -3);
  assert_forward(&oldarray[k]);
  assert_i32(&newarray[k], 6262);

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
  oldn = find_str_node(oldnode, oldhmask, key);
  assert(oldn != NULL);

  lj_tab_resize(L, t, t->asize, lj_fls(oldhmask) + 2u);
  newnode = lj_tab_node_acq(t);
  newhmask = lj_tab_node_hmask_acq(newnode);
  assert(newnode != oldnode);
  newn = find_str_node(newnode, newhmask, key);
  assert(newn != NULL);
  store_forward(&oldn->val);
  la_store32_rel(&lj_tab_node_hdrw(oldnode)->flags, 0);
  lj_tab_hmask_rel(t, oldhmask);
  lj_tab_node_rel(t, oldnode);

  setstrV(L, L->top, key);
  incr_top(L);
  lua_pushinteger(L, 7272);
  lua_rawset(L, -3);
  assert_forward(&oldn->val);
  assert_i32(&newn->val, 7272);

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
  oldn = find_str_node(oldnode, oldhmask, key);
  assert(oldn != NULL);

  lj_tab_resize(L, t, t->asize, lj_fls(oldhmask) + 2u);
  newnode = lj_tab_node_acq(t);
  newhmask = lj_tab_node_hmask_acq(newnode);
  assert(newnode != oldnode);
  newn = find_str_node(newnode, newhmask, key);
  assert(newn != NULL);
  store_forward(&oldn->val);
  la_store32_rel(&lj_tab_node_hdrw(oldnode)->flags, 0);
  lj_tab_hmask_rel(t, oldhmask);
  lj_tab_node_rel(t, oldnode);

  lua_pushinteger(L, 8282);
  lua_setfield(L, -2, name);
  assert_forward(&oldn->val);
  assert_i32(&newn->val, 8282);

  lj_tab_node_rel(t, newnode);
  lj_tab_hmask_rel(t, newhmask);
  lj_tab_node_hdr_flags_or_rel(oldnode, TABNODE_FLAG_RETIRING);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);

  exercise_direct_cas(L);
  exercise_meta_forward_retry(L);
  exercise_capi_rawseti_forward_retry(L);
  exercise_capi_settable_forward_retry(L);
  exercise_capi_rawset_forward_retry(L);
  exercise_capi_setfield_forward_retry(L);

  lua_close(L);
  printf("t-tab-cas-store OK: CAS table stores preserve FORWARD slots\n");
  return 0;
}
