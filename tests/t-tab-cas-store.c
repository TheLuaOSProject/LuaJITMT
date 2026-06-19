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

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);

  exercise_direct_cas(L);
  exercise_meta_forward_retry(L);

  lua_close(L);
  printf("t-tab-cas-store OK: CAS table stores preserve FORWARD slots\n");
  return 0;
}
