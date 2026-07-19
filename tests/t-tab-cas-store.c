/*
** Focused regression test for CAS-published table slot stores.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

/* Built by the M5 harness with LJ_TAB_TEST_HELPERS enabled. */

#define WRITER_ITERS 40000

typedef struct WriterArg {
  lua_State *L;
  TValue *slot;
  int32_t base;
} WriterArg;

typedef struct PostCasStaleCtx {
  lua_State *L;
  GCtab *table;
  TValue *oldarray;
  TValue *newarray;
  GCtab *value;
  TValue key_value;
  MSize newasize;
  int32_t key;
  uint8_t called;
} PostCasStaleCtx;

static PostCasStaleCtx postcas_stale;

static void postcas_make_array_stale(lua_State *L, GCtab *t, TValue *dst,
				     cTValue *key, cTValue *value)
{
  TGState *tg = L2TG(L);
  assert(L == postcas_stale.L && t == postcas_stale.table);
  assert(dst == &postcas_stale.oldarray[postcas_stale.key]);
  assert(lj_obj_equal(key, &postcas_stale.key_value));
  assert(tvistab(value) && tabV(value) == postcas_stale.value);
  assert(tg != NULL &&
         lj_gc2_rootdesc_snapshot(&tg->root_desc, NULL) ==
           LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE);
  assert(!postcas_stale.called);
  postcas_stale.called = 1;
  lj_tab_asize_rel(t, postcas_stale.newasize);
  lj_tab_array_rel(t, postcas_stale.newarray);
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

static void exercise_keyed_cas_array_stale(lua_State *L)
{
  GCtab *t;
  TValue *oldarray, *newarray, key, src, nilv, observed;
  MSize oldasize, newasize;
  int32_t k = 4;
  MSize i;

  lua_settop(L, 0);
  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  assert((MSize)k < oldasize);
  for (i = 0; i < oldasize; i++)
    lj_tab_storeint(L, lj_tab_setint(L, t, (int32_t)i), (int32_t)i + 12000);

  lj_tab_resize(L, t, (uint32_t)oldasize + 8u, 0);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  assert(newarray != oldarray);

  setintV(&key, k);
  setintV(&src, 12345);
  setnilV(&nilv);
  /* RETIRING is published before separated-array owner copy. Re-expose that
  ** root tuple with a still-nil successor and ensure neither delete nor
  ** put-if-absent can linearize before the per-slot FORWARD handoff. */
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);
  lj_tab_storenilraw(&newarray[k]);
  assert(lj_tab_trystoretv_cas_keyed(L, t, &newarray[k], &key, &nilv) ==
	 LJ_TAB_STORE_CAS_STALE);
  assert(lj_tab_trysetnil_cas_keyed(L, t, &newarray[k], &key, &src,
				    &observed) == LJ_TAB_STORE_CAS_STALE);
  assert(lj_tv_isnil_acq(&newarray[k]));
  lj_tab_storeint(L, &newarray[k], k + 12000);
  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
  assert(lj_tab_trystoretv_cas_keyed(L, t, &oldarray[k], &key, &src) ==
	 LJ_TAB_STORE_CAS_STALE);
  tabfwd_assert_i32(&oldarray[k], k + 12000);
  tabfwd_assert_i32(&newarray[k], k + 12000);
  assert(lj_tab_trystoretv_cas_keyed(L, t, lj_tab_setint(L, t, k),
				     &key, &src) == LJ_TAB_STORE_CAS_OK);
  tabfwd_assert_i32(&newarray[k], 12345);

  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
}

static void exercise_keyed_cas_hash_stale(lua_State *L)
{
  GCtab *t;
  GCstr *hkey;
  TValue keytv, src;
  Node *oldnode, *newnode, *oldn, *newn;
  MSize oldhmask, newhmask;

  lua_settop(L, 0);
  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  hkey = lj_str_newlit(L, "keyed_cas_hash_stale");
  lj_tab_storeint(L, lj_tab_setstr(L, t, hkey), 13000);
  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  oldn = tabfwd_find_str_node(oldnode, oldhmask, hkey);
  assert(oldn != NULL);

  lj_tab_resize(L, t, t->asize, lj_fls(oldhmask) + 2u);
  newnode = lj_tab_node_acq(t);
  newhmask = lj_tab_node_hmask_acq(newnode);
  assert(newnode != oldnode);
  newn = tabfwd_find_str_node(newnode, newhmask, hkey);
  assert(newn != NULL);

  setstrV(L, &keytv, hkey);
  setintV(&src, 13333);
  assert(lj_tab_trystoretv_cas_keyed(L, t, &oldn->val, &keytv, &src) ==
	 LJ_TAB_STORE_CAS_STALE);
  tabfwd_assert_forward(&oldn->val);
  tabfwd_assert_i32(&newn->val, 13000);
  assert(lj_tab_trystoretv_cas_keyed(L, t, lj_tab_setstr(L, t, hkey),
				     &keytv, &src) == LJ_TAB_STORE_CAS_OK);
  tabfwd_assert_i32(&newn->val, 13333);

  lj_tab_node_rel(t, newnode);
  lj_tab_hmask_rel(t, newhmask);
}

static void exercise_keyed_cas_hash_to_array_handoff(lua_State *L)
{
  GCtab *t, *value;
  TValue *oldarray, *newarray, key, src, nilv, observed, stored;
  Node *oldnode, *newnode;
  TValue *oldslot;
  MSize oldasize, newasize, oldhmask, newhmask;
  int32_t k;

  lua_settop(L, 0);
  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 8);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  k = (int32_t)oldasize + 4;
  lj_tab_storeint(L, lj_tab_setint(L, t, k), 14040);
  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  assert(oldhmask > 0);
  oldslot = tabfwd_find_num_slot(oldnode, oldhmask, k);
  assert(oldslot != NULL);
  tabfwd_assert_i32(oldslot, 14040);

  /* Keep a traced value live while resize moves k from hash to array. */
  lua_newtable(L);
  value = tabV(L->top-1);
  lj_tab_resize(L, t, (uint32_t)k + 8u, 0);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  newnode = lj_tab_node_acq(t);
  newhmask = lj_tab_node_hmask_acq(newnode);
  assert(newarray != oldarray && (MSize)k < newasize);
  assert(lj_tab_array_is_retiring(t, oldarray));
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);
  assert(lj_tab_node_is_retiring(oldnode));
  assert(lj_tab_node_nextgen_acq(oldnode) == newnode);
  tabfwd_assert_forward(oldslot);
  tabfwd_assert_i32(&newarray[k], 14040);

  /* Recreate the coherent pre-root-publication window. A hash FORWARD marker
  ** precedes successor-array value installation, so this tuple cannot safely
  ** authorize either delete or put-if-absent semantics. It must retry until
  ** the new array becomes the table root. */
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);
  lj_tab_node_rel(t, oldnode);
  lj_tab_hmask_rel(t, oldhmask);

  setintV(&key, k);
  setnilV(&nilv);
  /* Model the real freeze/key-publish/value-fill gap, in which the logical
  ** old hash edge is FORWARD but the successor value is still nil. */
  lj_tab_storenilraw(&newarray[k]);
  assert(lj_tab_trystoretv_cas_keyed(L, t, &newarray[k], &key, &nilv) ==
	 LJ_TAB_STORE_CAS_STALE);
  assert(lj_tv_isnil_acq(&newarray[k]));
  settabV(L, &src, value);
  assert(lj_tab_trysetnil_cas_keyed(L, t, &newarray[k], &key, &src,
				    &observed) ==
	 LJ_TAB_STORE_CAS_STALE);
  assert(lj_tv_isnil_acq(&newarray[k]));
  assert(lj_gc2_rootdesc_snapshot(&L2TG(L)->root_desc, NULL) ==
	 LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);

  lj_tab_storeint(L, &newarray[k], 14040);
  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
  lj_tab_node_rel(t, newnode);
  lj_tab_hmask_rel(t, newhmask);
  assert(lj_tab_trystoretv_cas_keyed(L, t, &newarray[k], &key, &src) ==
	 LJ_TAB_STORE_CAS_OK);
  lj_tv_load_acq(&stored, &newarray[k]);
  assert(tvistab(&stored) && tabV(&stored) == value);
  assert(lj_gc2_rootdesc_snapshot(&L2TG(L)->root_desc, NULL) ==
	 LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);

  lj_tv_load_acq(&stored, lj_tab_getint(t, k));
  assert(tvistab(&stored) && tabV(&stored) == value);
}

static void exercise_keyed_cas_array_to_hash_handoff(lua_State *L)
{
  GCtab *t, *value;
  TValue *oldarray, *newarray, *newslot, key, src, nilv, observed, stored;
  GCstr *sidekey;
  Node *oldnode, *newnode;
  MSize oldasize, newasize, oldhmask, newhmask;
  uint32_t shrink_asize = LJ_MAX_COLOSIZE + 4u;
  int32_t k;

  lua_settop(L, 0);
  lua_createtable(L, LJ_MAX_COLOSIZE + 24, 8);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  assert(oldasize > shrink_asize + 2u);
  k = (int32_t)oldasize - 2;
  lj_tab_storeint(L, &oldarray[k], 15050);
  sidekey = lj_str_newlit(L, "array_to_hash_handoff_side");
  lj_tab_storeint(L, lj_tab_setstr(L, t, sidekey), 15151);
  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  assert(oldhmask > 0);

  /* Keep a traced value live while resize moves k from array to hash. */
  lua_newtable(L);
  value = tabV(L->top-1);
  lj_tab_resize(L, t, shrink_asize, 0);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  newnode = lj_tab_node_acq(t);
  newhmask = lj_tab_node_hmask_acq(newnode);
  assert(newarray != oldarray && newasize == shrink_asize);
  assert((MSize)k >= newasize && newhmask > 0);
  assert(lj_tab_array_is_retiring(t, oldarray));
  assert(lj_tab_array_nextgen_acq(oldarray) == newarray);
  assert(newnode != oldnode && lj_tab_node_is_retiring(oldnode));
  assert(lj_tab_node_nextgen_acq(oldnode) == newnode);
  /* Separated-array migration deliberately retains the old edge; RETIRING,
  ** rather than a per-slot FORWARD marker, is the handoff certificate. */
  tabfwd_assert_i32(&oldarray[k], 15050);
  newslot = tabfwd_find_num_slot(newnode, newhmask, k);
  assert(newslot != NULL);
  tabfwd_assert_i32(newslot, 15050);

  /* Re-expose both retiring roots from the owner migration window. The old
  ** hash does not contain this array key, and its next pointer alone does not
  ** prove the successor value installation is complete. */
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);
  lj_tab_node_rel(t, oldnode);
  lj_tab_hmask_rel(t, oldhmask);

  setintV(&key, k);
  setnilV(&nilv);
  lj_tab_storenilraw(newslot);
  assert(lj_tab_trystoretv_cas_keyed(L, t, newslot, &key, &nilv) ==
	 LJ_TAB_STORE_CAS_STALE);
  assert(lj_tv_isnil_acq(newslot));
  settabV(L, &src, value);
  assert(lj_tab_trysetnil_cas_keyed(L, t, newslot, &key, &src, &observed) ==
	 LJ_TAB_STORE_CAS_STALE);
  assert(lj_tv_isnil_acq(newslot));

  /* Hash publication follows all migration stores and is the safe cross-part
  ** certificate while the old array root remains retiring. */
  lj_tab_storeint(L, newslot, 15050);
  lj_tab_node_rel(t, newnode);
  lj_tab_hmask_rel(t, newhmask);
  assert(lj_tab_trystoretv_cas_keyed(L, t, newslot, &key, &src) ==
	 LJ_TAB_STORE_CAS_OK);
  lj_tv_load_acq(&stored, newslot);
  assert(tvistab(&stored) && tabV(&stored) == value);
  assert(lj_gc2_rootdesc_snapshot(&L2TG(L)->root_desc, NULL) ==
	 LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);

  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
  lj_tv_load_acq(&stored, lj_tab_getint(t, k));
  assert(tvistab(&stored) && tabV(&stored) == value);
  tabfwd_assert_str_i32(t, sidekey, 15151);
}

static void exercise_guarded_commit_then_stale(lua_State *L)
{
  GCtab *t, *value;
  TValue *oldarray, *newarray;
  TValue key, src, committed;
  LJGC2TabStamp *stamp;
  uint32_t dirty0, dirty1;
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
    lj_tab_storeint(L, &oldarray[i], (int32_t)i + 15000);

  lj_tab_resize(L, t, (uint32_t)oldasize + 8u, 0);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  assert(newarray != oldarray && lj_tab_array_nextgen_acq(oldarray) == newarray);

  /* Re-expose the old vector as a coherent synthetic current generation. The
  ** post-CAS hook publishes the prepared next generation before currentness. */
  la_store32_rel(&lj_tab_array_hdrw(oldarray)->acap,
		 lj_tab_array_hdr_pack_acap(oldacap, 0));
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);

  lua_newtable(L);
  value = tabV(L->top-1);
  setintV(&key, k);
  settabV(L, &src, value);
  stamp = lj_arena_gc2_stamp_acq(t);
  assert(stamp != NULL);
  dirty0 = (uint32_t)la_load64_acq(&stamp->state);
  assert(dirty0 != UINT32_MAX);

  postcas_stale.L = L;
  postcas_stale.table = t;
  postcas_stale.oldarray = oldarray;
  postcas_stale.newarray = newarray;
  postcas_stale.value = value;
  postcas_stale.key_value = key;
  postcas_stale.newasize = newasize;
  postcas_stale.key = k;
  postcas_stale.called = 0;
  lj_tab_test_set_store_post_cas_hook(postcas_make_array_stale);
  assert(lj_tab_trystoretv_cas_keyed(L, t, &oldarray[k], &key, &src) ==
	 LJ_TAB_STORE_CAS_STALE);
  lj_tab_test_set_store_post_cas_hook(NULL);
  assert(postcas_stale.called);
  lj_tv_load_acq(&committed, &oldarray[k]);
  assert(tvistab(&committed) && tabV(&committed) == value);
  assert(lj_gc2_rootdesc_snapshot(&L2TG(L)->root_desc, NULL) ==
	 LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
  assert(lj_tab_gc2_rescan_state_acq(t) == LJ_TAB_RESCAN_COUNTED);
  dirty1 = (uint32_t)la_load64_acq(&stamp->state);
  assert(dirty1 == dirty0 + 1u);

  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
  lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);
  memset(&postcas_stale, 0, sizeof(postcas_stale));
}

static void exercise_helper_stores_ignore_side_mirrors(lua_State *L)
{
  GCtab *t;
  GCstr *hkey;
  TValue *array, keytv, src;
  Node *node, *hn;
  MSize asize, hmask;
  int32_t k = 5;

  lua_settop(L, 0);
  lua_createtable(L, LJ_MAX_COLOSIZE + 24, 8);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));
  asize = lj_tab_array_snapshot_acq(t, &array);
  assert((MSize)k < asize);

  lj_tab_storeint(L, lj_tab_setint(L, t, k), 15000);
  hkey = lj_str_newlit(L, "helper_store_side_mirror");
  lj_tab_storeint(L, lj_tab_setstr(L, t, hkey), 16000);
  node = lj_tab_node_snapshot_acq(t, &hmask);
  assert(hmask > 0);
  hn = tabfwd_find_str_node(node, hmask, hkey);
  assert(hn != NULL);

  lj_tab_asize_rel(t, 0);
  lj_tab_hmask_rel(t, 0);

  setintV(&keytv, k);
  setintV(&src, 15151);
  assert(lj_tab_trystoretv_cas_keyed(L, t, &array[k], &keytv, &src) ==
	 LJ_TAB_STORE_CAS_OK);
  tabfwd_assert_i32(&array[k], 15151);

  setintV(&src, 15252);
  assert(lj_tab_storetv_forjit_array_nogc(L, t, &array[k], &src,
					  (MSize)k) == &array[k]);
  tabfwd_assert_i32(&array[k], 15252);

  setstrV(L, &keytv, hkey);
  setintV(&src, 16161);
  assert(lj_tab_trystoretv_cas_keyed(L, t, &hn->val, &keytv, &src) ==
	 LJ_TAB_STORE_CAS_OK);
  tabfwd_assert_i32(&hn->val, 16161);

  setintV(&src, 16262);
  assert(lj_tab_storetv_forjit_hash(L, t, &hn->val, &src, &keytv) ==
	 &hn->val);
  tabfwd_assert_i32(&hn->val, 16262);

  lj_tab_asize_rel(t, asize);
  lj_tab_hmask_rel(t, hmask);
}

static void exercise_keyed_nil_cas_hash_stale(lua_State *L)
{
  GCtab *t;
  GCstr *hkey;
  TValue keytv, old, src, *oldslot, *newslot;
  Node *oldnode, *newnode;
  MSize oldhmask, newhmask;

  lua_settop(L, 0);
  lua_createtable(L, 0, 8);
  t = tabV(L->top-1);
  hkey = lj_str_newlit(L, "keyed_nil_cas_hash_stale");
  oldslot = lj_tab_setstr(L, t, hkey);
  assert(lj_tv_isnil_acq(oldslot));
  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);

  lj_tab_resize(L, t, t->asize, lj_fls(oldhmask) + 2u);
  newnode = lj_tab_node_acq(t);
  newhmask = lj_tab_node_hmask_acq(newnode);
  assert(newnode != oldnode);

  setstrV(L, &keytv, hkey);
  setintV(&src, 14444);
  assert(lj_tab_trysetnil_cas_keyed(L, t, oldslot, &keytv, &src, &old) ==
	 LJ_TAB_STORE_CAS_STALE);
  assert(lj_tv_isnil_acq(oldslot));

  newslot = lj_tab_setstr(L, t, hkey);
  assert(lj_tab_trysetnil_cas_keyed(L, t, newslot, &keytv, &src, &old) ==
	 LJ_TAB_STORE_CAS_OK);
  tabfwd_assert_i32(newslot, 14444);
  assert(lj_tab_trysetnil_cas_keyed(L, t, newslot, &keytv, &src, &old) ==
	 LJ_TAB_STORE_CAS_EXISTS);
  tabfwd_assert_i32(&old, 14444);

  lj_tab_node_rel(t, newnode);
  lj_tab_hmask_rel(t, newhmask);
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
  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);

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
  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);

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
  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);

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
  /* A hash FORWARD marker precedes successor-value installation, so the
  ** mutator waits for root publication before using the new slot. */
  lj_tab_node_rel(t, newnode);
  lj_tab_hmask_rel(t, newhmask);

  setstrV(L, L->top, key);
  incr_top(L);
  lua_pushinteger(L, 7272);
  lua_rawset(L, -3);
  tabfwd_assert_forward(&oldn->val);
  tabfwd_assert_i32(&newn->val, 7272);

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
  lj_tab_node_rel(t, newnode);
  lj_tab_hmask_rel(t, newhmask);

  lua_pushinteger(L, 8282);
  lua_setfield(L, -2, name);
  tabfwd_assert_forward(&oldn->val);
  tabfwd_assert_i32(&newn->val, 8282);

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
  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);

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
  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);

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

static void exercise_tsetm_helper_current_fast(lua_State *L)
{
  GCtab *t;
  TValue src[3];
  uint32_t calls0;

  lua_settop(L, 0);
  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));

  setintV(&src[0], 1111);
  setintV(&src[1], 2222);
  setintV(&src[2], 3333);

  lj_tab_test_reset_tsetm_fast_calls();
  calls0 = lj_tab_test_tsetm_fast_calls();
  lj_tab_storetvn_forvm_array(L, t, 4, src, 3);
  assert(lj_tab_test_tsetm_fast_calls() == calls0 + 1u);
  tabfwd_assert_i32(lj_tab_getint(t, 4), 1111);
  tabfwd_assert_i32(lj_tab_getint(t, 5), 2222);
  tabfwd_assert_i32(lj_tab_getint(t, 6), 3333);
}

static void exercise_tsetm_helper_entering_fallback(lua_State *L)
{
  global_State *g = G(L);
  GCtab *t;
  TValue src[2];
  uint32_t calls0;

  lua_settop(L, 0);
  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));

  setintV(&src[0], 4444);
  setintV(&src[1], 5555);

  lj_tab_test_reset_tsetm_fast_calls();
  calls0 = lj_tab_test_tsetm_fast_calls();
  assert(mt_entering_add_rlx(g, 1) == 0);
  lj_tab_storetvn_forvm_array(L, t, 7, src, 2);
  assert(mt_entering_sub_acqrel(g, 1) == 1);
  mt_entering_futex_wake(g, 0x7fffffff);

  assert(lj_tab_test_tsetm_fast_calls() == calls0);
  tabfwd_assert_i32(lj_tab_getint(t, 7), 4444);
  tabfwd_assert_i32(lj_tab_getint(t, 8), 5555);
}

static void exercise_tsetm_helper_current_retiring(lua_State *L)
{
  GCtab *t;
  TValue *oldarray, *newarray;
  TValue src[3], srcjit;
  TValue *stored;
  uint32_t wait0;
  MSize oldasize, newasize;
  int32_t start = 5;
  int32_t jitkey = start + 3;
  MSize i;

  lua_settop(L, 0);
  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  assert((MSize)jitkey < oldasize);
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

  for (i = 0; i < 3; i++) {
    lj_tab_storeint(L, &oldarray[start + i], (int32_t)(start + i) + 8100);
    lj_tab_storenilraw(&newarray[start + i]);
  }
  setintV(&src[0], 8181);
  setintV(&src[1], 8282);
  setintV(&src[2], 8383);
  {
    TValue keytv;
    setintV(&keytv, start);
    assert(lj_tab_trystoretv_cas_keyed(L, t, &newarray[start], &keytv,
				       &src[0]) == LJ_TAB_STORE_CAS_STALE);
  }
  /* FORWARD/RETIRING cannot prove that every copier with a captured value has
  ** finished. Publish the completed owner generation before semantic stores. */
  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
  lj_tab_test_reset_wait_no_l_calls();
  lj_tab_storetvn_forvm_array(L, t, (uint32_t)start, src, 3);
  wait0 = lj_tab_test_wait_no_l_calls();
  assert(wait0 == 0);
  for (i = 0; i < 3; i++)
    tabfwd_assert_i32(&oldarray[start + i], (int32_t)(start + i) + 8100);
  tabfwd_assert_i32(&newarray[start], 8181);
  tabfwd_assert_i32(&newarray[start + 1], 8282);
  tabfwd_assert_i32(&newarray[start + 2], 8383);

  lj_tab_storeint(L, &oldarray[jitkey], jitkey + 9000);
  lj_tab_storenilraw(&newarray[jitkey]);
  setintV(&srcjit, 8484);
  stored = lj_tab_storetv_forjit_array_nogc(L, t, &oldarray[jitkey],
					    &srcjit, (MSize)jitkey);
  assert(lj_tab_test_wait_no_l_calls() == wait0);
  assert(stored == &newarray[jitkey]);
  tabfwd_assert_i32(&oldarray[jitkey], jitkey + 9000);
  tabfwd_assert_i32(&newarray[jitkey], 8484);

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
  remembered0 = gc2_remembered_barriers_acq(g);
  lj_tab_storetvn_forvm_array(L, t, 4, src, 2);
  assert(gc2_remembered_barriers_acq(g) > remembered0);
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
  lj_tab_node_rel(reg, newnode);
  lj_tab_hmask_rel(reg, newhmask);

  assert(luaL_newmetatable(L, name) == 1);
  tabfwd_assert_forward(&oldn->val);
  lj_tv_load_acq(&nv, &newn->val);
  assert(tvistab(&nv));
  assert(tabV(&nv) == tabV(L->top-1));

  lj_tab_node_hdr_flags_or_rel(oldnode, TABNODE_FLAG_RETIRING);
  lua_pop(L, 1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);

  exercise_direct_cas(L);
  exercise_keyed_cas_array_stale(L);
  exercise_keyed_cas_hash_stale(L);
  exercise_keyed_cas_hash_to_array_handoff(L);
  exercise_keyed_cas_array_to_hash_handoff(L);
  exercise_guarded_commit_then_stale(L);
  exercise_helper_stores_ignore_side_mirrors(L);
  exercise_keyed_nil_cas_hash_stale(L);
  exercise_meta_forward_retry(L);
  exercise_capi_rawseti_forward_retry(L);
  exercise_capi_settable_forward_retry(L);
  exercise_capi_rawset_forward_retry(L);
  exercise_capi_setfield_forward_retry(L);
  exercise_table_insert_forward_retry(L);
  exercise_tsetm_helper_current_fast(L);
  exercise_tsetm_helper_entering_fallback(L);
  exercise_tsetm_helper_forward_retry(L);
  exercise_tsetm_helper_current_retiring(L);
  exercise_tsetm_helper_post_barrier(L);
  exercise_luaL_newmetatable_forward_retry(L);

  lua_close(L);
  printf("t-tab-cas-store OK: CAS stores fail closed until root publication\n");
  return 0;
}
