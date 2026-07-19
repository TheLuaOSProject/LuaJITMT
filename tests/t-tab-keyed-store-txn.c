/*
** Focused prepared exact keyed table-store transaction tests.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_gc2.h"
#include "lj_gc.h"
#include "lj_obj.h"
#include "lj_state.h"
#include "lj_tab.h"
#include "lj_tabtxn.h"
#include "lj_tg.h"
#include "lj_thr.h"

#include "lib/tab_forward_helpers.h"

#ifndef LJ_TAB_TEST_HELPERS
#error "t-tab-keyed-store-txn requires LJ_TAB_TEST_HELPERS"
#endif

typedef struct TxnPostCasStaleCtx {
  lua_State *L;
  GCtab *table;
  TValue *oldarray;
  TValue *newarray;
  GCtab *value;
  TValue key_value;
  MSize newasize;
  int32_t key;
  uint8_t called;
} TxnPostCasStaleCtx;

static TxnPostCasStaleCtx postcas_stale;

static void postcas_publish_successor(LJTabKeyedStoreTxn *txn)
{
  assert(txn != NULL && txn->owner_L == postcas_stale.L);
  assert(txn->parent == postcas_stale.table);
  assert(txn->dst == &postcas_stale.oldarray[postcas_stale.key]);
  assert(lj_obj_equal(&txn->key, &postcas_stale.key_value));
  assert(tvistab(&txn->desired) &&
	 tabV(&txn->desired) == postcas_stale.value);
  assert(lj_gc2_rootdesc_snapshot(&L2TG(postcas_stale.L)->root_desc, NULL) ==
	 LJ_GC2_ROOTDESC_SNAPSHOT_ACTIVE);
  assert(!postcas_stale.called);
  postcas_stale.called = 1;
  lj_tab_asize_rel(postcas_stale.table, postcas_stale.newasize);
  lj_tab_array_rel(postcas_stale.table, postcas_stale.newarray);
}

static uint32_t smr_readers(lua_State *L)
{
  return gc2_smr_readers_acq(G(L));
}

static void assert_txn_prepared(lua_State *L, uint32_t baseline)
{
  assert(smr_readers(L) == baseline + 1u);
}

static void assert_txn_released(lua_State *L, uint32_t baseline)
{
  assert(smr_readers(L) == baseline);
  assert(lj_gc2_rootdesc_snapshot(&L2TG(L)->root_desc, NULL) ==
	 LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
}

static TValue *array_slot(lua_State *L, GCtab *t, int32_t key,
			  TValue *keytv)
{
  setintV(keytv, key);
  return lj_tab_setint(L, t, key);
}

static void assert_slot_raw(TValue *slot, cTValue *want)
{
  TValue got;
  lj_tv_load_acq(&got, slot);
  assert(tv_rawload(&got) == tv_rawload(want));
}

static uintptr_t slot_addr(TValue *slot)
{
  return (uintptr_t)(void *)slot;
}

static void store_rooted(lua_State *L, GCtab *t, TValue *slot, cTValue *value)
{
  (void)lj_tab_storetv(L, slot, value);
  lj_gc_pubtabtv(L, t, value);
}

static void raw_replace(TValue *slot, cTValue *from, cTValue *to)
{
  TValue expected = *from;
  assert(lj_tv_cas(slot, &expected, to));
}

static void exercise_snapshot_replace(lua_State *L, GCtab *t)
{
  LJTabKeyedStoreTxn txn;
  TValue key, oldv, newv;
  TValue *slot = array_slot(L, t, 1, &key);
  uint32_t readers0 = smr_readers(L);
  int status = -1;

  lua_newtable(L);
  settabV(L, &oldv, tabV(L->top-1));
  lua_newtable(L);
  settabV(L, &newv, tabV(L->top-1));
  store_rooted(L, t, slot, &oldv);

  lj_tab_keyed_store_txn_init(&txn);
  assert(lj_tab_keyed_store_prepare_snapshot(L, &txn, t, slot_addr(slot), &key,
					      &newv) == LJ_TAB_STORE_CAS_OK);
  assert_txn_prepared(L, readers0);
  assert(lj_tab_keyed_store_expected(&txn) != NULL);
  assert(tv_rawload(lj_tab_keyed_store_expected(&txn)) == tv_rawload(&oldv));
  assert(lj_tab_keyed_store_commit(&txn, &status));
  assert(status == LJ_TAB_STORE_CAS_OK);
  assert(lj_tab_keyed_store_finish(L, &txn));
  assert_txn_released(L, readers0);
  assert_slot_raw(slot, &newv);
  lua_pop(L, 2);
}

static void exercise_changed_and_forward(lua_State *L, GCtab *t)
{
  LJTabKeyedStoreTxn txn;
  TValue key, oldv, newv, winner;
  TValue *slot = array_slot(L, t, 2, &key);
  uint32_t readers0 = smr_readers(L);
  int status = -1;

  setintV(&oldv, 20);
  setintV(&newv, 21);
  setintV(&winner, 22);
  store_rooted(L, t, slot, &oldv);
  lj_tab_keyed_store_txn_init(&txn);
  assert(lj_tab_keyed_store_prepare_exact(L, &txn, t, slot_addr(slot), &key,
					   &oldv,
					   &newv) == LJ_TAB_STORE_CAS_OK);
  assert_txn_prepared(L, readers0);
  raw_replace(slot, &oldv, &winner);  /* Exact competing publisher. */
  assert(!lj_tab_keyed_store_commit(&txn, &status));
  assert(status == LJ_TAB_STORE_CAS_CHANGED);
  assert(lj_tab_keyed_store_abort(L, &txn));
  assert_txn_released(L, readers0);
  assert_slot_raw(slot, &winner);

  store_rooted(L, t, slot, &oldv);
  lj_tab_keyed_store_txn_init(&txn);
  assert(lj_tab_keyed_store_prepare_exact(L, &txn, t, slot_addr(slot), &key,
					   &oldv,
					   &newv) == LJ_TAB_STORE_CAS_OK);
  assert_txn_prepared(L, readers0);
  tabfwd_store_forward(slot);
  assert(!lj_tab_keyed_store_commit(&txn, &status));
  assert(status == LJ_TAB_STORE_CAS_FORWARD);
  assert(lj_tab_keyed_store_abort(L, &txn));
  assert_txn_released(L, readers0);
  store_rooted(L, t, slot, &oldv);
}

static void exercise_stale_generation(lua_State *L, GCtab *t)
{
  LJTabKeyedStoreTxn txn;
  TValue key, oldv, newv;
  TValue *oldslot = array_slot(L, t, 3, &key);
  uintptr_t oldslot_addr = slot_addr(oldslot);
  uint32_t readers0 = smr_readers(L);

  setintV(&oldv, 30);
  setintV(&newv, 31);
  store_rooted(L, t, oldslot, &oldv);
  /* Preparation sees an opaque stale candidate.  It must reject it without
  ** dereferencing the retired vector or retaining transaction authority. */
  lj_tab_resize(L, t, lj_tab_asize_acq(t) + 32u, 0);
  lj_tab_keyed_store_txn_init(&txn);
  assert(lj_tab_keyed_store_prepare_exact(L, &txn, t, oldslot_addr, &key,
					   &oldv,
					   &newv) == LJ_TAB_STORE_CAS_STALE);
  assert_txn_released(L, readers0);
  assert_slot_raw(lj_tab_setint(L, t, 3), &oldv);
}

static void exercise_exact_delete(lua_State *L, lua_State *wrong, GCtab *t)
{
  LJTabKeyedStoreTxn txn;
  TGState *owner_tg = L2TG(L);
  LJStateClaim wrong_claim;
  TValue key, oldv, replacement, winner, nilv, observed;
  TValue *slot = array_slot(L, t, 4, &key);
  uint32_t readers0 = smr_readers(L);
  int status = -1;

  lua_newtable(L);
  settabV(L, &oldv, tabV(L->top-1));
  lua_newtable(L);
  settabV(L, &replacement, tabV(L->top-1));
  setintV(&winner, 404);
  setnilV(&nilv);

  store_rooted(L, t, slot, &oldv);
  lj_tab_keyed_store_txn_init(&txn);
  assert(lj_tab_keyed_store_prepare_exact(L, &txn, t, slot_addr(slot), &key,
					   &oldv,
					   &nilv) == LJ_TAB_STORE_CAS_OK);
  assert_txn_prepared(L, readers0);
  raw_replace(slot, &oldv, &winner);  /* Replacement wins before delete. */
  assert(!lj_tab_keyed_store_commit(&txn, &status));
  assert(status == LJ_TAB_STORE_CAS_CHANGED);
  /* Even a real same-actor/same-TG resume claim is not the exact state
  ** which acquired this TLS-accounted reader. */
  assert(lj_state_resumeclaim(wrong, lj_tg_tid_acq(owner_tg), &wrong_claim));
  assert(!lj_tab_keyed_store_abort(wrong, &txn));
  assert_txn_prepared(L, readers0);
  lj_state_dropresumeclaim(&wrong_claim);
  assert(lj_tab_keyed_store_expected(&txn) != NULL);
  assert(lj_tab_keyed_store_abort(L, &txn));
  assert_txn_released(L, readers0);
  assert_slot_raw(slot, &winner);

  store_rooted(L, t, slot, &replacement);
  lj_tv_load_acq(&observed, slot);
  lj_tab_keyed_store_txn_init(&txn);
  assert(lj_tab_keyed_store_prepare_exact(L, &txn, t, slot_addr(slot), &key,
					   &observed, &nilv) ==
	 LJ_TAB_STORE_CAS_OK);
  assert_txn_prepared(L, readers0);
  assert(lj_tab_keyed_store_commit(&txn, &status));
  assert(status == LJ_TAB_STORE_CAS_OK);
  assert(lj_tab_keyed_store_finish(L, &txn));
  assert_txn_released(L, readers0);
  assert(lj_tv_isnil_acq(slot));
  lua_pop(L, 2);
}

static void exercise_hash_slots(lua_State *L)
{
  int top = lua_gettop(L);
  LJTabKeyedStoreTxn txn;
  GCtab *t;
  Node *oldnode;
  TValue key, stale_key, oldv, newv, winner, nilv, stale_old, stale_new;
  TValue *slot, *stale_slot;
  uintptr_t stale_addr;
  MSize oldhmask;
  uint32_t readers0;
  int32_t hash_key = -100000003;
  int32_t stale_hash_key = -100000019;
  int status = -1;

  lua_createtable(L, 0, 4);
  t = tabV(L->top-1);
  setintV(&key, hash_key);
  slot = lj_tab_setint(L, t, hash_key);
  lua_newtable(L);
  settabV(L, &oldv, tabV(L->top-1));
  lua_newtable(L);
  settabV(L, &newv, tabV(L->top-1));
  setintV(&winner, 9001);
  setnilV(&nilv);
  store_rooted(L, t, slot, &oldv);
  readers0 = smr_readers(L);

  /* jit.attach uses these integer hash slots, not the small array part. */
  lj_tab_keyed_store_txn_init(&txn);
  assert(lj_tab_keyed_store_prepare_snapshot(L, &txn, t, slot_addr(slot),
					      &key, &newv) ==
	 LJ_TAB_STORE_CAS_OK);
  assert_txn_prepared(L, readers0);
  assert(lj_tab_keyed_store_commit(&txn, &status));
  assert(status == LJ_TAB_STORE_CAS_OK);
  assert(lj_tab_keyed_store_finish(L, &txn));
  assert_txn_released(L, readers0);
  assert_slot_raw(slot, &newv);

  lj_tab_keyed_store_txn_init(&txn);
  assert(lj_tab_keyed_store_prepare_exact(L, &txn, t, slot_addr(slot),
					   &key, &newv, &nilv) ==
	 LJ_TAB_STORE_CAS_OK);
  assert_txn_prepared(L, readers0);
  raw_replace(slot, &newv, &winner);
  assert(!lj_tab_keyed_store_commit(&txn, &status));
  assert(status == LJ_TAB_STORE_CAS_CHANGED);
  assert(lj_tab_keyed_store_abort(L, &txn));
  assert_txn_released(L, readers0);
  assert_slot_raw(slot, &winner);

  store_rooted(L, t, slot, &newv);
  lj_tab_keyed_store_txn_init(&txn);
  assert(lj_tab_keyed_store_prepare_exact(L, &txn, t, slot_addr(slot),
					   &key, &newv, &nilv) ==
	 LJ_TAB_STORE_CAS_OK);
  assert_txn_prepared(L, readers0);
  assert(lj_tab_keyed_store_commit(&txn, &status));
  assert(status == LJ_TAB_STORE_CAS_OK);
  assert(lj_tab_keyed_store_finish(L, &txn));
  assert_txn_released(L, readers0);
  assert(lj_tv_isnil_acq(slot));

  setintV(&stale_key, stale_hash_key);
  setintV(&stale_old, 19);
  setintV(&stale_new, 20);
  stale_slot = lj_tab_setint(L, t, stale_hash_key);
  store_rooted(L, t, stale_slot, &stale_old);
  stale_addr = slot_addr(stale_slot);  /* Integerize under the live root. */
  oldnode = lj_tab_node_acq(t);
  oldhmask = lj_tab_node_hmask_acq(oldnode);
  assert(oldhmask > 0);
  lj_tab_resize(L, t, lj_tab_asize_acq(t), lj_fls(oldhmask) + 2u);
  lj_tab_keyed_store_txn_init(&txn);
  assert(lj_tab_keyed_store_prepare_exact(L, &txn, t, stale_addr,
					   &stale_key, &stale_old,
					   &stale_new) ==
	 LJ_TAB_STORE_CAS_STALE);
  assert_txn_released(L, readers0);
  assert_slot_raw(lj_tab_setint(L, t, stale_hash_key), &stale_old);
  lua_settop(L, top);
}

static void exercise_committed_stale(lua_State *L)
{
  int top = lua_gettop(L);
  LJTabKeyedStoreTxn txn;
  LJGC2TabStamp *stamp;
  GCtab *t, *value;
  TValue *oldarray, *newarray;
  TValue key, expected, desired, current;
  MSize oldasize, newasize, oldacap, i;
  uint32_t dirty0, dirty1, readers0;
  int32_t k = 6;
  int status = -1;

  lua_createtable(L, LJ_MAX_COLOSIZE + 16, 0);
  t = tabV(L->top-1);
  assert(lj_tab_array_separated(t));
  oldarray = lj_tab_array_acq(t);
  oldasize = lj_tab_asize_acq(t);
  oldacap = lj_tab_acap_acq(t);
  assert((MSize)k < oldasize);
  for (i = 0; i < oldasize; i++)
    lj_tab_storeint(L, &oldarray[i], (int32_t)i + 15000);

  lj_tab_resize(L, t, (uint32_t)oldasize + 8u, 0);
  newarray = lj_tab_array_acq(t);
  newasize = lj_tab_asize_acq(t);
  assert(newarray != oldarray && lj_tab_array_nextgen_acq(oldarray) == newarray);

  /* Re-expose a coherent old generation; the post-CAS hook publishes the
  ** already-built successor before commit's closing currentness proof. */
  la_store32_rel(&lj_tab_array_hdrw(oldarray)->acap,
		 lj_tab_array_hdr_pack_acap(oldacap, 0));
  lj_tab_asize_rel(t, oldasize);
  lj_tab_array_rel(t, oldarray);

  lua_newtable(L);
  value = tabV(L->top-1);
  setintV(&key, k);
  setintV(&expected, k + 15000);
  settabV(L, &desired, value);
  stamp = lj_arena_gc2_stamp_acq(t);
  assert(stamp != NULL);
  dirty0 = (uint32_t)la_load64_acq(&stamp->state);
  assert(dirty0 != UINT32_MAX);
  readers0 = smr_readers(L);

  postcas_stale.L = L;
  postcas_stale.table = t;
  postcas_stale.oldarray = oldarray;
  postcas_stale.newarray = newarray;
  postcas_stale.value = value;
  postcas_stale.key_value = key;
  postcas_stale.newasize = newasize;
  postcas_stale.key = k;
  postcas_stale.called = 0;
  lj_tab_keyed_store_test_set_post_cas_hook(postcas_publish_successor);

  lj_tab_keyed_store_txn_init(&txn);
  assert(lj_tab_keyed_store_prepare_exact(L, &txn, t,
					   slot_addr(&oldarray[k]), &key,
					   &expected, &desired) ==
	 LJ_TAB_STORE_CAS_OK);
  assert_txn_prepared(L, readers0);
  assert(lj_tab_keyed_store_commit(&txn, &status));
  assert(status == LJ_TAB_STORE_CAS_STALE);
  assert(postcas_stale.called);
  lj_tab_keyed_store_test_set_post_cas_hook(NULL);
  assert(lj_tab_keyed_store_finish(L, &txn));
  assert_txn_released(L, readers0);

  lj_tv_load_acq(&current, &oldarray[k]);
  assert(tvistab(&current) && tabV(&current) == value);
  assert_slot_raw(&newarray[k], &expected);
  assert(lj_tab_gc2_rescan_state_acq(t) == LJ_TAB_RESCAN_COUNTED);
  dirty1 = (uint32_t)la_load64_acq(&stamp->state);
  assert(dirty1 == dirty0 + 1u);

  /* Leave the synthetic generation graph in its ordinary retired shape. */
  lj_tab_array_rel(t, newarray);
  lj_tab_asize_rel(t, newasize);
  lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);
  lua_settop(L, top);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  lua_State *wrong;
  GCtab *t;
  assert(L != NULL);
  luaL_openlibs(L);
  wrong = lua_newthread(L);  /* Same universe/TG, different state claim. */
  assert(wrong != NULL);
  lua_createtable(L, 96, 0);
  t = tabV(L->top-1);

  exercise_snapshot_replace(L, t);
  exercise_changed_and_forward(L, t);
  exercise_stale_generation(L, t);
  exercise_exact_delete(L, wrong, t);
  exercise_hash_slots(L);
  exercise_committed_stale(L);

  lua_close(L);
  puts("t-tab-keyed-store-txn OK");
  return 0;
}
