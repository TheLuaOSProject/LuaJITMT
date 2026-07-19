/*
** Table handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Major portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lj_tab_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_err.h"
#include "lj_state.h"
#include "lj_arena.h"
#include "lj_safepoint.h"
#include "lj_tab.h"
#include "lj_tg.h"
#include "lj_thr.h"
#if LJ_HASFFI
#include "lj_cdata.h"
#endif

#define LJ_TAB_MAXCHAIN		8u

#if defined(LJ_GC2_TEST_HELPERS)
static uint32_t tab_test_forjit_lease_pause;
static uint32_t tab_test_forjit_lease_paused;
static uint32_t tab_test_forjit_lease_release;
static uint32_t tab_test_forjit_snapshot_pause;
static uint32_t tab_test_forjit_snapshot_paused;
static uint32_t tab_test_forjit_snapshot_release;
static uint32_t tab_test_forjit_initial_miss;
static uint32_t tab_test_forjit_result_pause;
static uint32_t tab_test_forjit_result_paused;
static uint32_t tab_test_forjit_result_release;

void lj_tab_test_forjit_lease_pause(void)
{
  la_store32_rel(&tab_test_forjit_lease_release, 0);
  la_store32_rel(&tab_test_forjit_lease_paused, 0);
  la_store32_rel(&tab_test_forjit_lease_pause, 1);
}

uint32_t lj_tab_test_forjit_lease_paused(void)
{
  return la_load32_acq(&tab_test_forjit_lease_paused);
}

void lj_tab_test_forjit_lease_release(void)
{
  la_store32_rel(&tab_test_forjit_lease_release, 1);
}

void lj_tab_test_forjit_snapshot_pause(void)
{
  la_store32_rel(&tab_test_forjit_snapshot_release, 0);
  la_store32_rel(&tab_test_forjit_snapshot_paused, 0);
  la_store32_rel(&tab_test_forjit_snapshot_pause, 1);
}

uint32_t lj_tab_test_forjit_snapshot_paused(void)
{
  return la_load32_acq(&tab_test_forjit_snapshot_paused);
}

void lj_tab_test_forjit_snapshot_release(void)
{
  la_store32_rel(&tab_test_forjit_snapshot_release, 1);
}

void lj_tab_test_forjit_initial_miss_once(void)
{
  la_store32_rel(&tab_test_forjit_initial_miss, 1);
}

void lj_tab_test_forjit_result_pause(void)
{
  la_store32_rel(&tab_test_forjit_result_release, 0);
  la_store32_rel(&tab_test_forjit_result_paused, 0);
  la_store32_rel(&tab_test_forjit_result_pause, 1);
}

uint32_t lj_tab_test_forjit_result_paused(void)
{
  return la_load32_acq(&tab_test_forjit_result_paused);
}

void lj_tab_test_forjit_result_release(void)
{
  la_store32_rel(&tab_test_forjit_result_release, 1);
}

static int tab_forjit_test_take_initial_miss(void)
{
  return la_xchg32_acqrel(&tab_test_forjit_initial_miss, 0) != 0;
}

static void tab_forjit_test_pause_after_leases(void)
{
  if (!la_load32_acq(&tab_test_forjit_lease_pause))
    return;
  la_store32_rel(&tab_test_forjit_lease_paused, 1);
  while (!la_load32_acq(&tab_test_forjit_lease_release))
    la_cpu_pause();
  la_store32_rel(&tab_test_forjit_lease_paused, 0);
  la_store32_rel(&tab_test_forjit_lease_pause, 0);
}

static void tab_forjit_test_pause_after_integral_array(void)
{
  if (!la_load32_acq(&tab_test_forjit_snapshot_pause))
    return;
  la_store32_rel(&tab_test_forjit_snapshot_paused, 1);
  while (!la_load32_acq(&tab_test_forjit_snapshot_release))
    la_cpu_pause();
  la_store32_rel(&tab_test_forjit_snapshot_paused, 0);
  la_store32_rel(&tab_test_forjit_snapshot_pause, 0);
}

static void tab_forjit_test_pause_after_result_lease(cTValue *result)
{
  if (!tvisgcv(result) || !la_load32_acq(&tab_test_forjit_result_pause))
    return;
  la_store32_rel(&tab_test_forjit_result_paused, 1);
  while (!la_load32_acq(&tab_test_forjit_result_release))
    la_cpu_pause();
  la_store32_rel(&tab_test_forjit_result_paused, 0);
  la_store32_rel(&tab_test_forjit_result_pause, 0);
}
#else
#define tab_forjit_test_pause_after_leases() ((void)0)
#define tab_forjit_test_pause_after_integral_array() ((void)0)
#define tab_forjit_test_pause_after_result_lease(result) ((void)(result))
#define tab_forjit_test_take_initial_miss() 0
#endif

/* -- Object hashing ------------------------------------------------------ */

/* Hash an arbitrary key against a previously acquired hash vector snapshot. */
static Node *hashkey_node(Node *node, MSize hmask, cTValue *key)
{
  lj_assertX(!tvisint(key), "attempt to hash integer");
  if (tvisstr(key))
    return hashstr_node(node, hmask, strV(key));
  else if (tvisnum(key))
    return hashnum_node(node, hmask, key);
  else if (tvisbool(key))
    return hashmask_node(node, hmask, boolV(key));
  else
    return hashgcref_node(node, hmask, key->gcr);
}

static LJ_AINLINE int tab_nextnode_cas(Node *n, Node **expect, Node *want)
{
  uint64_t old = (uint64_t)(uintptr_t)(*expect);
  int ok = la_cas64(&n->next.ptr64, &old, (uint64_t)(uintptr_t)want,
		    LA_ACQ_REL, LA_ACQ);
  if (!ok)
    *expect = (Node *)(void *)(uintptr_t)old;
  return ok;
}

static LJ_AINLINE int tab_val_isclaim(cTValue *tv, cTValue *claim)
{
  return tv_rawload(tv) == tv_rawload(claim);
}

static LJ_AINLINE int tab_key_islocked(cTValue *key)
{
  return tviskeylock(key);
}

#ifdef LJ_TAB_TEST_HELPERS
static LJTabConstructorPrepublishHook tab_test_constructor_prepublish_hook;

void lj_tab_test_set_constructor_prepublish_hook(
  LJTabConstructorPrepublishHook hook)
{
  tab_test_constructor_prepublish_hook = hook;
}

static LJ_AINLINE void tab_test_constructor_prepublish(lua_State *L, GCtab *t)
{
  if (tab_test_constructor_prepublish_hook)
    tab_test_constructor_prepublish_hook(L, t);
}

#define TAB_TEST_COUNTER(name, hitfn) \
static uint32_t tab_test_##name; \
static LJ_AINLINE void tab_test_##hitfn(void) \
{ \
  (void)la_add32_acqrel(&tab_test_##name, 1); \
} \
uint32_t lj_tab_test_##name(void) \
{ \
  return la_load32_acq(&tab_test_##name); \
} \
void lj_tab_test_reset_##name(void) \
{ \
  la_store32_rel(&tab_test_##name, 0); \
}

TAB_TEST_COUNTER(wait_no_l_calls, wait_no_l_call)
#else
#define tab_test_wait_no_l_call()	((void)0)
#define tab_test_constructor_prepublish(L, t)	((void)0)
#endif

LJ_FUNCA void lj_tab_wait_no_l(void)
{
  tab_test_wait_no_l_call();
  (void)lj_thr_retry_yield(NULL);
}

LJ_FUNCA void lj_tab_wait_l(lua_State *L)
{
  int had_stopreq = lj_safepoint_had_stopreq(L);
  uint32_t actions;
  /*
  ** L-aware table retry waits make C/API callers native and safepoint-visible
  ** while preserving the no-state helper for VM/JIT/internal paths where only
  ** TLS ownership is known.
  */
  actions = lj_thr_retry_yield(L);
  lj_safepoint_checkstop_fresh(L, actions, had_stopreq);
}

void lj_tab_read_enter(TGState *tg)
{
  uint32_t depth;
  if (!tg || !tg->gl)
    abort();
  depth = lj_tg_tab_read_depth_acq(tg);
  if (depth == ~(uint32_t)0)
    abort();
  if (depth == 0) {
    /* Publish the epoch before depth. A reclaimer which acquire-observes the
    ** nonzero depth can therefore classify this reader without sampling an
    ** uninitialized/stale outer epoch. The raw generation is acquired only by
    ** the caller after this release publication. */
    lj_tg_tab_read_epoch_rel(tg, gc2_hs_epoch_acq(tg->gl));
  }
  lj_tg_tab_read_depth_rel(tg, depth + 1u);
}

void lj_tab_read_leave(TGState *tg)
{
  uint32_t depth;
  if (!tg)
    abort();
  depth = lj_tg_tab_read_depth_acq(tg);
  if (depth == 0)
    abort();
  lj_tg_tab_read_depth_rel(tg, depth - 1u);
  if (depth == 1u) {
    /* Depth is the reader-active publication. Clear it before the now-stale
    ** outer epoch, matching protected-unwind ordering: a reclaimer which sees
    ** zero depth ignores either side of the following diagnostic reset. The
    ** TG owner cannot begin another outer scope concurrently with itself. */
    lj_tg_tab_read_epoch_rel(tg, 0);
  }
}

void lj_tab_read_checkpoint(TGState *tg, LJTabReadCheckpoint *cp)
{
  uint32_t depth;
  if (!tg || !tg->gl || !cp)
    abort();
  depth = lj_tg_tab_read_depth_acq(tg);
  cp->tg = tg;
  cp->depth = depth;
  cp->epoch = depth != 0 ? lj_tg_tab_read_epoch_acq(tg) : 0;
}

void lj_tab_read_unwind(const LJTabReadCheckpoint *cp)
{
  TGState *tg;
  uint32_t depth;
  uint64_t epoch;
  if (!cp || !(tg = cp->tg) || !tg->gl)
    abort();
  depth = lj_tg_tab_read_depth_acq(tg);
  if (depth < cp->depth)
    abort();
  if (cp->depth != 0) {
    epoch = lj_tg_tab_read_epoch_acq(tg);
    /* Nested readers inherit the outer epoch. A different epoch while the
    ** saved scope is still active proves that a non-owner reset or an
    ** unmatched leave corrupted the publication. */
    if (epoch != cp->epoch)
      abort();
    lj_tg_tab_read_epoch_rel(tg, cp->epoch);
    lj_tg_tab_read_depth_rel(tg, cp->depth);
  } else {
    /* Release the active flag first. Reclaimers ignore epoch whenever depth
    ** is zero, so clearing the diagnostic/stale epoch afterwards cannot make
    ** a retired generation eligible while a reader is still advertised. */
    lj_tg_tab_read_depth_rel(tg, 0);
    lj_tg_tab_read_epoch_rel(tg, 0);
  }
}

static uint32_t tab_struct_tid(lua_State *L)
{
  TGState *tg = L ? L2TG(L) : lj_thr_get_tg();
  uint32_t tid = tg ? lj_tg_tid_acq(tg) : 0;
  return tid != 0 ? tid : ~(uint32_t)0;
}

#ifdef LJ_TAB_TEST_HELPERS
TAB_TEST_COUNTER(struct_owner_l_waits, struct_owner_l_wait)
TAB_TEST_COUNTER(struct_owner_no_l_waits, struct_owner_no_l_wait)
TAB_TEST_COUNTER(struct_enter_acquires, struct_enter_acquire)
TAB_TEST_COUNTER(new0_calls, new0_call)
TAB_TEST_COUNTER(clear_shared_calls, clear_shared_call)
TAB_TEST_COUNTER(tsetm_fast_calls, tsetm_fast_call)
TAB_TEST_COUNTER(vm_array_store_calls, vm_array_store_call)
TAB_TEST_COUNTER(vm_strhash_store_calls, vm_strhash_store_call)
#undef TAB_TEST_COUNTER
#else
#define tab_test_struct_owner_l_wait()			((void)0)
#define tab_test_struct_owner_no_l_wait()		((void)0)
#define tab_test_struct_enter_acquire()			((void)0)
#define tab_test_new0_call()				((void)0)
#define tab_test_clear_shared_call()			((void)0)
#define tab_test_tsetm_fast_call()			((void)0)
#define tab_test_vm_array_store_call()			((void)0)
#define tab_test_vm_strhash_store_call()		((void)0)
#endif

static void tab_struct_owner_wait(lua_State *L, GCtab *t, uint32_t owner)
{
  UNUSED(t);
  UNUSED(owner);
  if (L) {
    tab_test_struct_owner_l_wait();
    lj_tab_wait_l(L);
  } else {
    tab_test_struct_owner_no_l_wait();
    lj_tab_wait_no_l();
  }
}

static LJ_AINLINE int tab_mt_concurrent(void)
{
  TGState *tg = lj_thr_get_tg();
  global_State *g = tg ? tg->gl : NULL;
  return g && (mt_live_acq(g) != 0 || mt_entering_acq(g) != 0);
}

static LJ_AINLINE int tab_private_mutation_allowed(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  /*
  ** KEYLOCK/CAS publication and structural ownership are needed once another
  ** Lua thread, an attaching thread, or GC2 workers can observe table vectors.
  ** Active marking also keeps the shared path responsible for publication
  ** barriers. Outside those windows the mutator is the only table observer.
  */
  return !mt_active_or_entering_acq(g) && gc2_n_workers_acq(g) == 0 &&
	 gc2_phase_acq(g) == LJ_GC2_IDLE && !lj_tg_mark_active_acq(tg);
}

int lj_tab_struct_enter(lua_State *L, GCtab *t)
{
  uint32_t tid = tab_struct_tid(L);
  for (;;) {
    uint32_t owner = lj_tab_struct_owner_acq(t);
    if (owner == tid)
      return 0;
    if (owner == 0) {
      uint32_t expect = 0;
      if (lj_tab_struct_owner_cas(t, &expect, tid)) {
	tab_test_struct_enter_acquire();
	return 1;
      }
      owner = expect;
    }
    tab_struct_owner_wait(L, t, owner);
  }
}

void lj_tab_struct_leave(GCtab *t, int acquired)
{
  if (acquired) {
    /*
    ** Waiters retry with acquire loads after cooperative yield. The release
    ** store publishes the structural mutation without a timed park/wake path.
    */
    lj_tab_struct_owner_rel(t, 0);
  }
}

static LJ_AINLINE int tab_hash_key_hidden(cTValue *key)
{
  return tvisnil(key) || tab_key_islocked(key);
}

static LJ_AINLINE int tab_key_retry_once(cTValue *key, int *retry)
{
  if (tab_key_islocked(key) && *retry) {
    *retry = 0;
    lj_tab_wait_no_l();
    return 1;
  }
  return 0;
}

static LJ_AINLINE int tab_key_read_retry_once(cTValue *key, int *retry)
{
  if (tab_key_islocked(key) && *retry) {
    *retry = 0;
    return 1;
  }
  return 0;
}

static LJ_AINLINE int tab_val_absent(cTValue *val)
{
  return tvisnil(val) || tvisforward(val);
}

static LJ_AINLINE int tab_tv_snapshot_valid(cTValue *tv)
{
  /*
  ** Resize and weak-clear scans read table slots without owning the old array or
  ** node generation. A racing clear/retire/reuse can leave a TValue whose tag
  ** names the old occupant while the acquired body header already belongs to a
  ** different GC type. Such a tag/header mismatch is not a live Lua edge and
  ** must not be copied into a new generation.
  */
  return lj_tv_gcref_type_match(tv);
}

static LJ_AINLINE int tab_tv_forjit_loadable(cTValue *tv)
{
  /*
  ** Helper-backed JIT reads feed a VLOAD from a temporary TValue. Never expose
  ** table-internal sentinels or stale GC snapshots as ordinary Lua values.
  */
  return !tvistabinternal(tv) && tab_tv_snapshot_valid(tv);
}

static LJ_AINLINE int tab_val_is_publish_claim(cTValue *val)
{
#if LJ_HASFFI
  return lj_cdata_fin_isclaim(val);
#else
  UNUSED(val);
  return 0;
#endif
}

static LJ_AINLINE int tab_slot_absent_acq(const TValue *slot)
{
  TValue val;
  lj_tv_load_acq(&val, slot);
  return tab_val_absent(&val);
}

static LJ_AINLINE int tab_val_forward_retry(GCtab *t, cTValue *val, Node *node)
{
  if (tvisforward(val)) {
    Node *root = lj_tab_node_acq(t);
    if (root != node) {
      if (lj_tab_node_is_retiring(root))
	lj_tab_wait_no_l();
      return 1;
    }
    if (lj_tab_node_is_retiring(node)) {
      lj_tab_wait_no_l();
      return 1;
    }
  }
  return 0;
}

static LJ_AINLINE int tab_forwarded_lookup_retry(GCtab *t, Node *node)
{
  Node *root = lj_tab_node_acq(t);
  if (root != node) {
    if (lj_tab_node_is_retiring(root))
      lj_tab_wait_no_l();
    return 1;
  }
  if (lj_tab_node_is_retiring(node)) {
    lj_tab_wait_no_l();
    return 1;
  }
  return 0;
}

static LJ_AINLINE int tab_node_forward_hop(GCtab *t, Node **nodep,
					   MSize *hmaskp)
{
  Node *node = *nodep;
  Node *root = lj_tab_node_acq(t);
  /*
  ** The table root is the only stable publication point after a resize. Avoid
  ** touching an old generation header once the table no longer publishes it.
  */
  if (root != node) {
    *nodep = lj_tab_node_snapshot_acq(t, hmaskp);
    return 1;
  }
  {
    Node *next = lj_tab_node_nextgen_acq(node);
    if (next && next != node) {
      *nodep = next;
      *hmaskp = lj_tab_node_hmask_acq(next);
      return 1;
    }
  }
  return 0;
}

static LJ_AINLINE TValue *tab_forwarded_int_arrayslot(GCtab *t, int32_t key)
{
  TValue *array;
  MSize asize = lj_tab_array_snapshot_acq(t, &array);
  if ((MSize)key < asize)
    return &array[key];
  if (lj_tab_array_forward_hop(t, &array, &asize) && (MSize)key < asize)
    return &array[key];
  return NULL;
}

static LJ_AINLINE int tab_forwarded_hash_value(GCtab *t, Node **nodep,
					       MSize *hmaskp, cTValue *key,
					       TValue *valp)
{
  Node *n;
  if (!tab_node_forward_hop(t, nodep, hmaskp))
    return 0;
  if (tvisnum(key)) {
    int64_t i64;
    int32_t k;
    if (lj_num2int_check(numV(key), i64, k)) {
      cTValue *tv = lj_tab_getint(t, k);
      if (tv) {
	lj_tv_load_acq(valp, tv);
	return !tab_val_absent(valp);
      }
    }
  }
  n = hashkey_node(*nodep, *hmaskp, key);
  do {
    TValue nk;
    lj_tv_load_acq(&nk, &n->key);
    if (lj_obj_equal(&nk, key)) {
      lj_tv_load_acq(valp, &n->val);
      return !tab_val_absent(valp);
    }
  } while ((n = lj_tab_nextnode_acq(n)));
  return 0;
}

static TValue *tab_forwarded_setslot(GCtab *t, Node **nodep, MSize *hmaskp,
				     cTValue *key)
{
  Node *n;
  if (!tab_node_forward_hop(t, nodep, hmaskp))
    return NULL;
  if (tvisnum(key)) {
    int64_t i64;
    int32_t k;
    if (lj_num2int_check(numV(key), i64, k)) {
      TValue *slot = tab_forwarded_int_arrayslot(t, k);
      if (slot)
	return slot;
    }
  }
  if (*hmaskp == 0)
    return NULL;
  n = hashkey_node(*nodep, *hmaskp, key);
  do {
    TValue nk;
    lj_tv_load_acq(&nk, &n->key);
    if (lj_obj_equal(&nk, key))
      return &n->val;
    if (tab_key_islocked(&nk))
      return NULL;
  } while ((n = lj_tab_nextnode_acq(n)));
  return NULL;
}

TValue *lj_tab_forwarded_array_slot(GCtab *t, TValue *array, MSize asize,
				    MSize idx, TValue *valp)
{
  for (;;) {
    TValue *nextarray = array;
    MSize nextasize = asize;
    TValue *slot = NULL;
    if (!lj_tab_array_forward_hop_forward(t, &nextarray, &nextasize)) {
      if (lj_tab_array_is_colocated(t, array)) {
	if (lj_tab_array_acq(t) == array) {
	  lj_tab_wait_no_l();
	  asize = lj_tab_array_snapshot_acq(t, &array);
	  continue;
	}
	nextasize = lj_tab_array_snapshot_acq(t, &nextarray);
      } else {
	return NULL;
      }
    }
    if (idx < nextasize) {
      slot = &nextarray[idx];
    } else if (idx <= (MSize)INT32_MAX) {
      slot = (TValue *)lj_tab_getinth(t, (int32_t)idx);
    }
    if (slot) {
      lj_tv_load_acq(valp, slot);
      if (tvisforward(valp)) {
	array = nextarray;
	asize = nextasize;
	continue;
      }
      if (!tab_val_absent(valp))
	return slot;
    }
    if (lj_tab_array_acq(t) == array && lj_tab_array_is_retiring(t, array)) {
      lj_tab_wait_no_l();
      asize = lj_tab_array_snapshot_acq(t, &array);
      continue;
    }
    return NULL;
  }
}

TValue *lj_tab_forwarded_hash_slot(GCtab *t, Node *node, MSize hmask,
				   cTValue *key, TValue *valp)
{
  for (;;) {
    Node *hopnode = node;
    MSize hophmask = hmask;
    TValue *slot = tab_forwarded_setslot(t, &hopnode, &hophmask, key);
    if (slot) {
      lj_tv_load_acq(valp, slot);
      if (tvisforward(valp)) {
	node = hopnode;
	hmask = hophmask;
	continue;
      }
      if (!tab_val_absent(valp))
	return slot;
    }
    if (lj_tab_node_acq(t) == node && lj_tab_node_is_retiring(node)) {
      lj_tab_wait_no_l();
      node = lj_tab_node_snapshot_acq(t, &hmask);
      continue;
    }
    return NULL;
  }
}

static LJ_AINLINE int tab_array_slot_absent_acq(GCtab *t, TValue **arrayp,
						MSize *asizep, MSize idx)
{
  for (;;) {
    TValue val;
    TValue *array = *arrayp;
    lj_tv_load_acq(&val, &array[idx]);
    if (tvisforward(&val)) {
      TValue *oldarray = array;
      MSize nextasize = *asizep;
      TValue *nextarray = array;
      if (lj_tab_array_forward_hop_forward(t, &nextarray, &nextasize)) {
	if (idx < nextasize) {
	  TValue nextval;
	  lj_tv_load_acq(&nextval, &nextarray[idx]);
	  if (tvisnil(&nextval) && lj_tab_array_acq(t) == oldarray &&
	      (lj_tab_array_is_retiring(t, oldarray) ||
		       lj_tab_array_is_colocated(t, oldarray))) {
	    lj_tab_wait_no_l();
	    *asizep = lj_tab_array_snapshot_acq(t, arrayp);
	    continue;
	  }
	  *arrayp = nextarray;
	  *asizep = nextasize;
	  continue;
	}
      }
      if (lj_tab_array_acq(t) != array) {
	*asizep = lj_tab_array_snapshot_acq(t, arrayp);
	continue;
      }
      if (lj_tab_array_is_retiring(t, array) ||
	  lj_tab_array_is_colocated(t, array)) {
	lj_tab_wait_no_l();
	*asizep = lj_tab_array_snapshot_acq(t, arrayp);
	continue;
      }
    }
    return tab_val_absent(&val);
  }
}

static TValue *tab_findkey_or_keylock(Node *anchor, cTValue *key, int *locked,
				      MSize *chainlen)
{
  Node *n;
  MSize len = 0;
  *locked = 0;
  for (n = anchor; n != NULL; n = lj_tab_nextnode_acq(n)) {
    TValue nk;
    len++;
    lj_tv_load_acq(&nk, &n->key);
    if (lj_obj_equal(&nk, key)) {
      *chainlen = len;
      return &n->val;
    }
    if (tab_key_islocked(&nk)) {
      *chainlen = len;
      *locked = 1;
      return NULL;
    }
  }
  *chainlen = len;
  return NULL;
}

/* -- Table creation and destruction -------------------------------------- */

static LJ_AINLINE Node *tab_node_new(lua_State *L, MSize hmask)
{
  TabNodeHdr *hdr = (TabNodeHdr *)lj_mem_new(L, lj_tab_node_bytes(hmask));
  Node *node = (Node *)(void *)((char *)(void *)hdr + sizeof(TabNodeHdr));
  hdr->hmask = hmask;
  hdr->flags = (hmask + 1u) & TABNODE_FREECOUNT_MASK;
  setmref(hdr->next_gen, NULL);
  return node;
}

static LJ_AINLINE Node *tab_node_new_deferred_nothrow(lua_State *L,
						       MSize hmask)
{
  TabNodeHdr *hdr = (TabNodeHdr *)lj_mem_new_deferred_nothrow(
    L, lj_tab_node_bytes(hmask));
  Node *node;
  if (!hdr)
    return NULL;
  node = (Node *)(void *)((char *)(void *)hdr + sizeof(TabNodeHdr));
  hdr->hmask = hmask;
  hdr->flags = (hmask + 1u) & TABNODE_FREECOUNT_MASK;
  setmref(hdr->next_gen, NULL);
  return node;
}

static LJ_AINLINE int tab_valid_hmask(MSize hmask)
{
  MSize hsize;
  if (hmask > (((MSize)1u << LJ_MAX_HBITS) - 1u))
    return 0;
  hsize = hmask + 1u;
  return (hsize & hmask) == 0;
}

static LJ_AINLINE int tab_node_free_ready(const Node *node, MSize hmask)
{
  /*
  ** The hash-vector header is the physical allocation contract. Retired
  ** generations can outlive their publishing table, so the free boundary checks
  ** that the retire record and the vector header still describe the same valid
  ** allocation before subtracting the vector from GC accounting.
  */
  return node != NULL && tab_valid_hmask(hmask) &&
	 lj_tab_node_hmask_acq(node) == hmask;
}

static LJ_AINLINE void tab_node_free(global_State *g, Node *node, MSize hmask)
{
  if (LJ_UNLIKELY(node == &g->nilnode || !tab_node_free_ready(node, hmask)))
    return;
  lj_mem_free(g, lj_tab_node_hdrw(node), lj_tab_node_bytes(hmask));
}

static LJ_AINLINE TValue *tab_array_new(lua_State *L, MSize asize, MSize acap)
{
  TabArrayHdr *hdr = (TabArrayHdr *)lj_mem_new(L, lj_tab_array_bytes(acap));
  lj_tab_array_hdr_init(hdr, asize, acap);
  return lj_tab_array_slots(hdr);
}

static LJ_AINLINE TValue *tab_array_new_deferred_nothrow(lua_State *L,
							 MSize asize,
							 MSize acap)
{
  TabArrayHdr *hdr = (TabArrayHdr *)lj_mem_new_deferred_nothrow(
    L, lj_tab_array_bytes(acap));
  if (!hdr)
    return NULL;
  lj_tab_array_hdr_init(hdr, asize, acap);
  return lj_tab_array_slots(hdr);
}

static LJ_AINLINE int tab_valid_acap(MSize acap)
{
  return acap > 0 && acap <= LJ_MAX_ASIZE;
}

static LJ_AINLINE int tab_array_free_ready(const TValue *array, MSize acap)
{
  /*
  ** Array capacity is stored beside the slots instead of in the table header
  ** once a table has split from colocated storage. Match the retire/free size
  ** against that slot header so stale side-vector observations cannot turn into
  ** a bogus multi-gigabyte lj_mem_free().
  */
  return array != NULL && tab_valid_acap(acap) &&
	 lj_tab_array_hdr_acap_acq(array) == acap;
}

static LJ_AINLINE void tab_array_free(global_State *g, TValue *array, MSize acap)
{
  if (LJ_UNLIKELY(!tab_array_free_ready(array, acap)))
    return;
  lj_mem_free(g, lj_tab_array_hdrw(array), lj_tab_array_bytes(acap));
}

#ifdef LJ_TAB_TEST_HELPERS
int lj_tab_test_table_candidate(global_State *g, GCobj *o)
{
  /* Identity-only test probe: no object/vector pointer escapes this status
  ** call, so the temporary typed retention can be released on return. */
  return lj_gc2_markobj_expected_status(
    g, o, (uint32_t)~LJ_TTAB, NULL) >= 0;
}
#endif

static LJ_AINLINE int tab_gc_table_valid_held(const GCtab *t)
{
  return t && (uint32_t)la_load8_acq(&t->gct) == (uint32_t)~LJ_TTAB;
}

static LJ_AINLINE int tab_gc_array_hdr_valid(global_State *g,
					     const TValue *array,
					     MSize *asizep, MSize *acapp,
					     MSize *flagsp)
{
  const void *hdr;
  MSize asize, acap, flags;
  if (!array)
    return 0;
  hdr = (const void *)lj_tab_array_hdr(array);
  if (!lj_gc2_mem_registered_known(g, hdr) &&
      !lj_gc2_mem_registered_known_reclaim_held(g, hdr))
    return 0;
  asize = lj_tab_array_hdr_asize_acq(array);
  acap = lj_tab_array_hdr_acap_acq(array);
  flags = lj_tab_array_hdr_flags_acq(array);
  if (!tab_valid_acap(acap) || asize > acap)
    return 0;
  *asizep = asize;
  *acapp = acap;
  *flagsp = flags;
  return 1;
}

static int tab_array_snapshot_gc_impl(global_State *g, const GCtab *t,
				      TValue **arrayp, MSize *asizep,
				      MSize *acapp)
{
  TValue *array;
  MSize asize, acap, flags;
  if (!tab_gc_table_valid_held(t))
    return LJ_TAB_GC_SNAPSHOT_INVALID;
  array = lj_tab_array_acq(t);
  asize = lj_tab_asize_acq(t);
  acap = 0;
  if (!array) {
    if (asize != 0)
      return LJ_TAB_GC_SNAPSHOT_INVALID;
  } else if (lj_tab_array_is_colocated(t, array)) {
#if LJ_MAX_COLOSIZE != 0
    MSize colosz = lj_tab_colo_size(t);
    if (colosz > LJ_MAX_COLOSIZE || asize > colosz)
      return LJ_TAB_GC_SNAPSHOT_INVALID;
#endif
  } else {
    /*
    ** GC traversals can see stale gray-list entries and resize generations after
    ** the publishing mutator has moved on. Validate the side-vector allocation
    ** through the arena registry before reading its header. A currently published
    ** retiring generation is an in-flight resize, not something a collector
    ** should block on: callers reschedule the table and let the mutator finish
    ** publication.
    */
    if (!tab_gc_array_hdr_valid(g, array, &asize, &acap, &flags)) {
      if (lj_tab_array_acq(t) != array)
	return LJ_TAB_GC_SNAPSHOT_TRANSIENT;
      return LJ_TAB_GC_SNAPSHOT_INVALID;
    }
    if (lj_tab_array_acq(t) != array)
      return LJ_TAB_GC_SNAPSHOT_TRANSIENT;
    if (flags & TABARRAY_FLAG_RETIRING)
      return LJ_TAB_GC_SNAPSHOT_TRANSIENT;
  }
  *arrayp = array;
  *asizep = asize;
  *acapp = acap;
  return LJ_TAB_GC_SNAPSHOT_OK;
}

int lj_tab_array_snapshot_gc_held(global_State *g, const GCtab *t,
					  TValue **arrayp, MSize *asizep,
					  MSize *acapp)
{
  return tab_array_snapshot_gc_impl(g, t, arrayp, asizep, acapp);
}

static int tab_node_snapshot_gc_impl(global_State *g, const GCtab *t,
				     Node **nodep, MSize *hmaskp)
{
  Node *node;
  MSize hmask, flags;
  if (!tab_gc_table_valid_held(t))
    return LJ_TAB_GC_SNAPSHOT_INVALID;
  node = lj_tab_node_acq(t);
  if (!node)
    return LJ_TAB_GC_SNAPSHOT_INVALID;
  if (node != &g->nilnode &&
      !lj_gc2_mem_registered_known(g, (const void *)lj_tab_node_hdr(node)) &&
      !lj_gc2_mem_registered_known_reclaim_held(
	g, (const void *)lj_tab_node_hdr(node))) {
    if (lj_tab_node_acq(t) != node)
      return LJ_TAB_GC_SNAPSHOT_TRANSIENT;
    return LJ_TAB_GC_SNAPSHOT_INVALID;
  }
  hmask = lj_tab_node_hmask_acq(node);
  if (!tab_valid_hmask(hmask) && (node != &g->nilnode || hmask != 0)) {
    if (lj_tab_node_acq(t) != node)
      return LJ_TAB_GC_SNAPSHOT_TRANSIENT;
    return LJ_TAB_GC_SNAPSHOT_INVALID;
  }
  flags = lj_tab_node_hdr_flags_acq(node);
  if (lj_tab_node_acq(t) != node)
    return LJ_TAB_GC_SNAPSHOT_TRANSIENT;
  if (flags & TABNODE_FLAG_RETIRING)
    return LJ_TAB_GC_SNAPSHOT_TRANSIENT;
  *nodep = node;
  *hmaskp = hmask;
  return LJ_TAB_GC_SNAPSHOT_OK;
}

int lj_tab_node_snapshot_gc_held(global_State *g, const GCtab *t,
					 Node **nodep, MSize *hmaskp)
{
  return tab_node_snapshot_gc_impl(g, t, nodep, hmaskp);
}

static LJ_AINLINE Node *newhpart_alloc(lua_State *L, uint32_t hbits,
				       MSize *hmaskp)
{
  uint32_t i, hsize, hmask;
  Node *node;
  lj_assertL(hbits != 0, "zero hash size");
  if (hbits > LJ_MAX_HBITS)
    lj_err_msg(L, LJ_ERR_TABOV);
  hsize = 1u << hbits;
  hmask = hsize - 1u;
  node = tab_node_new(L, hmask);
  for (i = 0; i < hsize; i++) {
    Node *n = &node[i];
    lj_tab_nextnode_set(n, NULL);
    lj_tab_storenilraw(&n->key);
    lj_tab_storenilraw(&n->val);
  }
  *hmaskp = hmask;
  return node;
}

static LJ_AINLINE Node *newhpart_alloc_deferred_nothrow(lua_State *L,
							 uint32_t hbits,
							 MSize *hmaskp)
{
  uint32_t i, hsize, hmask;
  Node *node;
  if (hbits == 0 || hbits > LJ_MAX_HBITS)
    return NULL;
  hsize = 1u << hbits;
  hmask = hsize - 1u;
  node = tab_node_new_deferred_nothrow(L, hmask);
  if (!node)
    return NULL;
  for (i = 0; i < hsize; i++) {
    Node *n = &node[i];
    lj_tab_nextnode_set(n, NULL);
    lj_tab_storenilraw(&n->key);
    lj_tab_storenilraw(&n->val);
  }
  *hmaskp = hmask;
  return node;
}

static LJ_AINLINE void tab_pub_node_mem(lua_State *L, GCtab *t, Node *node)
{
  global_State *g = G(L);
  UNUSED(t);
  /* A replacement vector is private to this construction/resize attempt until
  ** the table root is release-published. Afterwards the table owns it. An
  ** ordinary collision with the opportunistic registry reclaimer therefore
  ** needs a root-scan retry, not a permanent NO_RECLAIM pin. */
  if (node)
    (void)lj_gc2_markmem_registered_publish_try(g,
						 lj_tab_node_hdrw(node));
}

static LJ_AINLINE void tab_pub_array_mem(lua_State *L, GCtab *t,
					 TValue *array, MSize acap)
{
  global_State *g = G(L);
  UNUSED(t);
  /* Same private-before/release-published-after lifetime as the hash vector. */
  if (array)
    (void)lj_gc2_markmem_registered_publish_try(g,
	      acap ? (void *)lj_tab_array_hdrw(array) : (void *)array);
}

static LJ_AINLINE void tab_node_freecount_set_private(Node *node,
						      MSize freecount)
{
  lj_tab_node_hdrw(node)->flags = freecount & TABNODE_FREECOUNT_MASK;
}

static LJ_AINLINE void newhpart_publish(lua_State *L, GCtab *t, Node *node,
					MSize hmask, Node *freetop,
					MSize freecount)
{
  /*
  ** Replacement hash vectors are private until the table root is release-pub-
  ** lished below. Rehash migration keeps the remaining capacity in a local
  ** counter and publishes it once, avoiding one CAS per migrated key.
  */
  tab_node_freecount_set_private(node, freecount);
  tab_pub_node_mem(L, t, node);
  setfreetop(t, node, freetop);
  lj_tab_node_rel(t, node);
  lj_tab_hmask_rel(t, hmask);
}

/* Create new hash part for table. */
static LJ_AINLINE void newhpart(lua_State *L, GCtab *t, uint32_t hbits)
{
  MSize hmask;
  Node *node = newhpart_alloc(L, hbits, &hmask);
  newhpart_publish(L, t, node, hmask, &node[hmask+1], hmask + 1u);
}

static LJ_AINLINE void tab_storekeyrel(lua_State *L, TValue *dst,
				       cTValue *key)
{
  TValue k;
  if (LJ_UNLIKELY(!tab_tv_snapshot_valid(key)))
    lj_err_msg(L, LJ_ERR_NILIDX);
  copyTV(L, &k, key);
  if (LJ_UNLIKELY(tvismzero(&k)))
    k.u64 = 0;
  copyTVrel(L, dst, &k);
}

static LJ_AINLINE int tab_weak_value_side(int weak)
{
  return (weak & LJ_GC_WEAKVAL) != 0;
}

static LJ_AINLINE int tab_weak_key_side(int weak)
{
  return (weak & LJ_GC_WEAKKEY) != 0;
}

static LJ_AINLINE int tab_resize_key_is_weak(int weak, cTValue *key)
{
  /*
  ** Strings are not weak references in LuaJIT's weak-table clearing rules; a
  ** string key therefore keeps the hash entry's value strongly reachable.
  */
  return tab_weak_key_side(weak) && key && tvisgcv(key) && !tvisstr(key);
}

static LJ_AINLINE int tab_resize_value_is_strong(int weak, cTValue *key)
{
  return !tab_weak_value_side(weak) && !tab_resize_key_is_weak(weak, key);
}

static LJ_AINLINE void tab_resize_pub_value(lua_State *L, GCtab *t,
					    int weak, cTValue *key,
					    cTValue *val)
{
  /*
  ** Resize migration preserves storage ownership; it must not upgrade a weak
  ** value edge or a value behind a weak collectable key into a semantic root.
  ** Strong value sides still need publication before the old generation is
  ** hidden or the new generation becomes visible.
  */
  if (tab_resize_value_is_strong(weak, key))
    lj_gc_pubroot(L, val);
  UNUSED(t);
}

static int tab_freeze_forward(lua_State *L, GCtab *t, int weak,
			      cTValue *key, TValue *slot, TValue *oldp)
{
  TValue forward;
  setforwardV(&forward);
  for (;;) {
    lj_tv_load_acq(oldp, slot);
    if (tvisforward(oldp))
      return 0;
    /* A FINREG claim is the keyed writer's logical slot pin. Moving it would
    ** strand an indistinguishable claim in the retired generation and let the
    ** claimant publish its result outside the table. The claim owner performs
    ** no resize-dependent work, so leave the slot and retry only after it has
    ** resolved. A later tranche replaces this cooperative retry with the
    ** table-wide nonblocking claim descriptor. */
    if (tab_val_is_publish_claim(oldp)) {
      lj_tab_wait_no_l();
      continue;
    }
    if (tvisnil(oldp))
      return 0;
    /*
    ** Freezing hides the old table edge until the replacement generation is
    ** published. Mark the value before the CAS so a concurrent collector cannot
    ** observe a temporary gap between old and new generations.
    */
    tab_resize_pub_value(L, t, weak, key, oldp);
    if (lj_tv_cas(slot, oldp, &forward))
      return 1;  /* M5: old slot ownership moved to its next generation. */
    lj_tab_wait_no_l();
  }
}

static int tab_freeze_forward_any(lua_State *L, GCtab *t, int weak,
				  TValue *slot, TValue *oldp)
{
  TValue forward;
  setforwardV(&forward);
  for (;;) {
    lj_tv_load_acq(oldp, slot);
    if (tvisforward(oldp))
      return 0;
    if (!tvisnil(oldp))
      tab_resize_pub_value(L, t, weak, NULL, oldp);
    if (lj_tv_cas(slot, oldp, &forward))
      return 1;
    lj_tab_wait_no_l();
  }
}

static int tab_store_if_absent_cas(lua_State *L, TValue *dst, cTValue *src)
{
  TValue old;
  UNUSED(L);
  for (;;) {
    lj_tv_load_acq(&old, dst);
    if (!tab_val_absent(&old))
      return 0;
    if (lj_tv_cas(dst, &old, src))
      return 1;
    if (!tab_val_absent(&old))
      return 0;
    lj_tab_wait_no_l();
  }
}

static void tab_migrate_store_if_absent(lua_State *L, GCtab *t, int weak,
					TValue *dst, cTValue *key,
					cTValue *val)
{
  if (LJ_UNLIKELY(!tab_tv_snapshot_valid(val)))
    return;
  if (key && !tab_resize_key_is_weak(weak, key))
    lj_gc_pubtabkey(L, t, key);
  tab_resize_pub_value(L, t, weak, key, val);
  if (tab_store_if_absent_cas(L, dst, val)) {
    /*
    ** The destination slot is now visible to table traversals. Run the normal
    ** table-value barrier only for strong value sides; weak-value/all-weak
    ** payloads remain candidates for the same weak clearing pass that would
    ** have seen them in the old generation.
    */
    if (tab_resize_value_is_strong(weak, key))
      lj_gc_pubtabtv(L, t, dst);
  }
}

static TValue *tab_rehash_insert(lua_State *L, Node *nodebase, MSize hmask,
				 Node **freetopp, MSize *freecountp,
				 cTValue *key)
{
  /* Destination belongs to an unpublished replacement hash vector. */
  Node *n = hashkey_node(nodebase, hmask, key);
  lj_assertL(*freecountp != 0, "no free node during rehash");
  (*freecountp)--;
  if (!lj_tv_isnil_acq(&n->val)) {
    Node *freenode = *freetopp;
    do {
      lj_assertL(freenode > nodebase, "no free node during rehash");
    } while (!lj_tv_isnil_acq(&(--freenode)->key));
    *freetopp = freenode;
    lj_tab_nextnode_set(freenode, lj_tab_nextnode_acq(n));
    tab_storekeyrel(L, &freenode->key, key);
    lj_tab_nextnode_set(n, freenode);
    return &freenode->val;
  }
  tab_storekeyrel(L, &n->key, key);
  return &n->val;
}

static int tab_rehash_arrayindex(uint32_t asize, cTValue *key, uint32_t *idxp)
{
  int64_t i64;
  int32_t k;
  if (tvisint(key)) {
    k = intV(key);
  } else if (tvisnum(key) && lj_num2int_check(numV(key), i64, k)) {
    /* Numeric key converted below. */
  } else {
    return 0;
  }
  if ((MSize)k < asize) {
    *idxp = (uint32_t)k;
    return 1;
  }
  return 0;
}

static TValue *tab_rehash_arrayslot(TValue *array, uint32_t asize,
				    cTValue *key)
{
  uint32_t idx;
  return tab_rehash_arrayindex(asize, key, &idx) ? &array[idx] : NULL;
}

static TValue *tab_rehash_slot(lua_State *L, TValue *array, uint32_t asize,
			       Node *nodebase, MSize hmask, Node **freetopp,
			       MSize *freecountp, cTValue *key)
{
  TValue *slot = tab_rehash_arrayslot(array, asize, key);
  return slot ? slot : tab_rehash_insert(L, nodebase, hmask, freetopp,
					 freecountp, key);
}

static int tab_resize_copy_hash_slot(lua_State *L, GCtab *t, Node *oldnode,
				     MSize oldhmask, MSize idx,
				     TValue *array, uint32_t asize,
				     Node *newnode, MSize newhmask,
				     Node **newfreetopp,
				     MSize *newfreecountp, int freeze_old,
				     int weak)
{
  Node *n;
  TValue key, val;
  TValue *slot;
  lj_assertL(oldhmask > 0 && idx <= oldhmask, "bad resize hash copy slot");
  n = &oldnode[idx];
  /*
  ** This helper is intentionally idempotent at the slot level: copied values
  ** are installed with put-if-absent semantics and old-generation FORWARD/nil
  ** values are clean no-ops. It is still called by the owner-driven resize path
  ** today, but it is the unit a future cooperative copy cursor can hand to
  ** helpers without changing publication semantics.
  */
  do {
    lj_tv_load_acq(&key, &n->key);
    if (!tab_key_islocked(&key))
      break;
    lj_tab_wait_no_l();
  } while (1);
  if (freeze_old) {
    if (!tab_freeze_forward(L, t, weak, &key, &n->val, &val))
      return 0;
  } else {
    lj_tv_load_acq(&val, &n->val);
  }
  if (tab_val_absent(&val) || !tab_tv_snapshot_valid(&val) ||
      !tab_tv_snapshot_valid(&key) || tab_hash_key_hidden(&key))
    return 0;
  if (newhmask > 0) {
    slot = tab_rehash_slot(L, array, asize, newnode, newhmask,
			   newfreetopp, newfreecountp, &key);
  } else {
    slot = tab_rehash_arrayslot(array, asize, &key);
    lj_assertL(slot != NULL, "missing hash part during rehash");
  }
  tab_migrate_store_if_absent(L, t, weak, slot, &key, &val);
  return 1;
}

static int tab_resize_copy_array_slot(lua_State *L, GCtab *t,
				      TValue *oldarray, uint32_t idx,
				      TValue *array, uint32_t asize,
				      Node *newnode, MSize newhmask,
				      Node **newfreetopp,
				      MSize *newfreecountp,
				      int freeze_nil_slots, int weak)
{
  TValue key, val;
  TValue *slot;
  /*
  ** Separated arrays keep nil slots nil while retiring only live slots.
  ** Colocated arrays must freeze every copied/tail slot, including nil, so a
  ** stale array snapshot cannot accept a write after the table has split or
  ** shrunk. Keep that semantic choice explicit at each call site.
  */
  if (freeze_nil_slots) {
    if (!tab_freeze_forward_any(L, t, weak, &oldarray[idx], &val))
      return 0;
  } else {
    /*
    ** Separated arrays are marked RETIRING before owner migration starts.
    ** Writers that race with the copy use the next-generation array, so the
    ** owner does not need to replace the old value with FORWARD here. Keeping
    ** the old edge visible until the new array is published prevents GC cycles
    ** that start mid-resize from missing values held only by this table.
    */
    lj_tv_load_acq(&val, &oldarray[idx]);
    if (tvisforward(&val))
      return 0;
    if (!tab_val_absent(&val))
      tab_resize_pub_value(L, t, weak, NULL, &val);
  }
  if (tab_val_absent(&val) || !tab_tv_snapshot_valid(&val))
    return 0;
  if (idx < asize) {
    lj_assertL(array != NULL, "missing array part during resize copy");
    slot = &array[idx];
    tab_migrate_store_if_absent(L, t, weak, slot, NULL, &val);
  } else {
    lj_assertL(newhmask > 0 && newnode != NULL,
	       "missing hash part during array tail rehash");
    setnumV(&key, (lua_Number)idx);
    slot = tab_rehash_insert(L, newnode, newhmask, newfreetopp,
			     newfreecountp, &key);
    tab_migrate_store_if_absent(L, t, weak, slot, &key, &val);
  }
  return 1;
}

#ifdef LJ_TAB_TEST_HELPERS
static TValue *tab_resize_assist_array_slot(lua_State *L, GCtab *t,
					    TValue *oldarray, MSize oldasize,
					    MSize idx)
{
  TValue *nextarray, *dst;
  MSize nextasize;
  TValue val;
  int weak;
  if (!oldarray || idx >= oldasize || lj_tab_array_is_colocated(t, oldarray) ||
      !lj_tab_array_is_retiring(t, oldarray))
    return NULL;
  /*
  ** Writers that catch a retiring separated array can help copy their exact
  ** slot before publishing into the successor. Restrict this helper to
  ** same-index array slots: tail-to-hash migration still needs the resize
  ** owner's free-node accounting and key materialization.
  */
  nextarray = lj_tab_array_nextgen_acq(oldarray);
  if (!nextarray || nextarray == oldarray ||
      lj_tab_array_is_colocated(t, nextarray))
    return NULL;
  nextasize = lj_tab_array_hdr_asize_acq(nextarray);
  if (idx >= nextasize)
    return NULL;  /* Tail migration still belongs to the resize owner. */
  weak = lj_gc2_weak_write_candidate(L, t);
  dst = &nextarray[idx];
  for (;;) {
    lj_tv_load_acq(&val, &oldarray[idx]);
    if (tvisforward(&val))
      return dst;
    if (tab_val_absent(&val)) {
      TValue forward, expect = val;
      setforwardV(&forward);
      (void)lj_tv_cas(&oldarray[idx], &expect, &forward);
      return dst;
    }
    if (tab_tv_snapshot_valid(&val)) {
      TValue forward, expect = val;
      tab_resize_pub_value(L, t, weak, NULL, &val);
      if (tab_store_if_absent_cas(L, dst, &val) &&
	  !tab_weak_value_side(weak))
	lj_gc_pubtabtv(L, t, dst);
      /*
      ** Once the successor has the value or a newer owner, the old slot can
      ** become a handoff marker. This is best-effort: losing the CAS leaves the
      ** old snapshot visible and the existing retry paths still preserve Lua
      ** semantics, but winning it lets later readers/writers hop without a wait.
      */
      setforwardV(&forward);
      (void)lj_tv_cas(&oldarray[idx], &expect, &forward);
      return dst;
    }
    return dst;
  }
}
#endif

#ifdef LJ_TAB_TEST_HELPERS
int lj_tab_test_resize_copy_hash_slot(lua_State *L, GCtab *src, MSize idx,
				      GCtab *dst, int freeze_old)
{
  MSize oldhmask, newhmask, newfreecount = 0;
  Node *oldnode = lj_tab_node_snapshot_acq(src, &oldhmask);
  Node *newnode = lj_tab_node_snapshot_acq(dst, &newhmask);
  Node *newfreetop = NULL;
  TValue *array;
  uint32_t asize = (uint32_t)lj_tab_array_snapshot_acq(dst, &array);
  int copied;
  int weak = lj_gc2_weak_write_candidate(L, dst);
  if (newhmask > 0) {
    newfreetop = getfreetop(dst, newnode);
    newfreecount = lj_tab_node_freecount_acq(newnode);
  }
  copied = tab_resize_copy_hash_slot(L, dst, oldnode, oldhmask, idx, array,
				     asize, newnode, newhmask, &newfreetop,
				     &newfreecount, freeze_old, weak);
  if (newhmask > 0) {
    setfreetop(dst, newnode, newfreetop);
    lj_tab_node_freecount_set_rel(newnode, newfreecount);
  }
  return copied;
}

int lj_tab_test_resize_copy_array_slot(lua_State *L, GCtab *src, uint32_t idx,
				       GCtab *dst, int freeze_nil_slots)
{
  MSize newhmask, newfreecount = 0;
  Node *newnode = lj_tab_node_snapshot_acq(dst, &newhmask);
  Node *newfreetop = NULL;
  TValue *oldarray = lj_tab_array_acq(src);
  TValue *array;
  uint32_t asize = (uint32_t)lj_tab_array_snapshot_acq(dst, &array);
  int copied;
  int weak = lj_gc2_weak_write_candidate(L, dst);
  if (newhmask > 0) {
    newfreetop = getfreetop(dst, newnode);
    newfreecount = lj_tab_node_freecount_acq(newnode);
  }
  copied = tab_resize_copy_array_slot(L, dst, oldarray, idx, array, asize,
				      newnode, newhmask, &newfreetop,
				      &newfreecount, freeze_nil_slots, weak);
  if (newhmask > 0) {
    setfreetop(dst, newnode, newfreetop);
    lj_tab_node_freecount_set_rel(newnode, newfreecount);
  }
  return copied;
}

TValue *lj_tab_test_resize_assist_array_slot(lua_State *L, GCtab *src,
					     uint32_t idx)
{
  TValue *array = lj_tab_array_acq(src);
  MSize asize = lj_tab_asize_acq(src);
  return tab_resize_assist_array_slot(L, src, array, asize, idx);
}
#endif

static uint32_t tab_rehash_hashcount(lua_State *L, Node *oldnode, MSize oldhmask,
				     uint32_t oldasize, uint32_t asize,
				     int *deadkeyp, int *retryp)
{
  uint32_t count = 0;
  int deadkey = 0;
  if (oldhmask > 0) {
    uint32_t i;
    for (i = 0; i <= oldhmask; i++) {
      Node *n = &oldnode[i];
      TValue key, val;
      uint32_t idx;
      lj_tv_load_acq(&key, &n->key);
      if (tab_key_islocked(&key)) {
	*retryp = 1;
	return 0;
      }
      lj_tv_load_acq(&val, &n->val);
      /* Claims pin both nil-key construction slots and established FINREG
      ** entries. Detect either before the resize publishes RETIRING; a claim
      ** racing after this scan is caught again by tab_freeze_forward(). */
      if (tab_val_is_publish_claim(&val)) {
	*retryp = 1;
	return 0;
      }
      if (tvisnil(&key)) {
	continue;
      }
      if (!tab_tv_snapshot_valid(&key) || !tab_tv_snapshot_valid(&val) ||
	  tab_hash_key_hidden(&key))
	continue;
      if (tab_val_absent(&val))
	deadkey = 1;
      if (!tab_rehash_arrayindex(asize, &key, &idx))
	count++;
    }
  }
  if (asize < oldasize)
    count += oldasize - asize;
  if (deadkeyp)
    *deadkeyp = deadkey;
  UNUSED(L);
  return count;
}

#define TAB_RETIRE_PUSH(name, type, head_acq, next_rel, head_cas) \
static void name(global_State *g, type *ret) \
{ \
  type *head = head_acq(g); \
  do { \
    next_rel(ret, head); \
  } while (!head_cas(g, &head, ret)); \
}

/* 06 section 6.3.5 raw node retire. */
TAB_RETIRE_PUSH(tab_retired_push, TabNodeRetire,
		lj_tab_node_retired_head_acq,
		lj_tab_node_retired_next_rel,
		lj_tab_node_retired_head_cas)

static LJ_AINLINE void tab_node_retired_tab_rel(TabNodeRetire *ret, GCtab *t)
{
  la_storeptr_rel((void **)&ret->tab, t);
}

static TabNodeRetire *tab_retire_reserve(lua_State *L)
{
  TabNodeRetire *ret = lj_mem_newt(L, sizeof(TabNodeRetire), TabNodeRetire);
  return ret;
}

static void tab_retire_init(lua_State *L, TabNodeRetire *ret, GCtab *t,
			    Node *node, MSize hmask)
{
  lj_assertL(tab_node_free_ready(node, hmask),
	     "mismatched retired table node size");
  tab_node_retired_tab_rel(ret, t);
  lj_tab_node_retired_node_rel(ret, node);
  lj_tab_node_retired_hmask_rel(ret, hmask);
  lj_tab_node_retired_epoch_rel(ret, 0);
  lj_tab_node_retired_armed_rel(ret, 0);
  lj_tab_node_retired_next_rel(ret, NULL);
}

static void tab_retire_discard(global_State *g, TabNodeRetire *ret)
{
  if (ret)
    lj_mem_freet(g, ret);
}

static void tab_retire_preserve(global_State *g, TabNodeRetire *ret)
{
  if (!lj_gc2_minor_roots_skip_bridge_mark(g)) {
    (void)lj_gc2_markmem_registered_publish_try(g, ret);
    (void)lj_gc2_markmem_registered_publish_try(
      g, lj_tab_node_hdrw(lj_tab_node_retired_node_acq(ret)));
  }
}

static void tab_retire_arm(global_State *g, TabNodeRetire *ret)
{
  GCtab *t = lj_tab_node_retired_tab_acq(ret);
  Node *node = lj_tab_node_retired_node_acq(ret);
  /*
  ** The retire record is pushed before the new hash generation is fully
  ** published, then armed after publication. A marker can run between those
  ** steps or before a just-pushed record reaches the retired-list scan, so the
  ** arming edge preserves both the record and the old storage around the
  ** release-published armed bit. A transient publication-marker miss is safe:
  ** before arming, the detached-list consumer must requeue the record; after
  ** arming, the newly published retire epoch cannot pass the grace test. The
  ** resize attempt also retains its direct pointers throughout this function.
  */
  tab_retire_preserve(g, ret);
  if (!t || lj_tab_node_acq(t) == node) {
    lj_assertG(0, "arming table node before replacement publication");
    abort();
  }
  lj_tab_node_retired_epoch_rel(ret, lj_gc2_retire_epoch(g));
  lj_tab_node_retired_armed_rel(ret, 1);
  tab_retire_preserve(g, ret);
  /* ret->tab is identity metadata, not a semantic GC edge. The cold reclaim
  ** check takes a counted non-marking lease before dereferencing it. */
}

/* 06 section 6.3.1 raw array retire. */
TAB_RETIRE_PUSH(tab_array_retired_push, TabArrayRetire,
		lj_tab_array_retired_head_acq,
		lj_tab_array_retired_next_rel,
		lj_tab_array_retired_head_cas)

#undef TAB_RETIRE_PUSH

static LJ_AINLINE void tab_array_retired_tab_rel(TabArrayRetire *ret, GCtab *t)
{
  la_storeptr_rel((void **)&ret->tab, t);
}

static TabArrayRetire *tab_array_retire_reserve(lua_State *L)
{
  TabArrayRetire *ret = lj_mem_newt(L, sizeof(TabArrayRetire), TabArrayRetire);
  return ret;
}

static void tab_array_retire_init(lua_State *L, TabArrayRetire *ret, GCtab *t,
				  TValue *array, MSize acap)
{
  lj_assertL(tab_array_free_ready(array, acap),
	     "mismatched retired table array capacity");
  tab_array_retired_tab_rel(ret, t);
  lj_tab_array_retired_array_rel(ret, array);
  lj_tab_array_retired_acap_rel(ret, acap);
  lj_tab_array_retired_epoch_rel(ret, 0);
  lj_tab_array_retired_armed_rel(ret, 0);
  lj_tab_array_retired_next_rel(ret, NULL);
}

static void tab_array_retire_discard(global_State *g, TabArrayRetire *ret)
{
  if (ret)
    lj_mem_freet(g, ret);
}

static void tab_array_retire_preserve(global_State *g, TabArrayRetire *ret)
{
  if (!lj_gc2_minor_roots_skip_bridge_mark(g)) {
    (void)lj_gc2_markmem_registered_publish_try(g, ret);
    (void)lj_gc2_markmem_registered_publish_try(
      g, lj_tab_array_hdrw(lj_tab_array_retired_array_acq(ret)));
  }
}

static void tab_array_retire_arm(global_State *g, TabArrayRetire *ret)
{
  GCtab *t = lj_tab_array_retired_tab_acq(ret);
  TValue *array = lj_tab_array_retired_array_acq(ret);
  /*
  ** Arrays use the same push-then-arm publication protocol as hash nodes.
  ** Preserve at the arming edge so a concurrent marker cannot miss storage that
  ** becomes reclaimable metadata after its retired-list pass.
  */
  tab_array_retire_preserve(g, ret);
  if (!t || lj_tab_array_acq(t) == array) {
    lj_assertG(0, "arming table array before replacement publication");
    abort();
  }
  lj_tab_array_retired_epoch_rel(ret, lj_gc2_retire_epoch(g));
  lj_tab_array_retired_armed_rel(ret, 1);
  tab_array_retire_preserve(g, ret);
  /* Owner identity remains nonsemantic, matching node retirement. */
}

static LJ_AINLINE int tab_retire_epoch_elapsed(uint64_t completed_epoch,
					       uint64_t retire_epoch)
{
  return completed_epoch >= retire_epoch &&
	 completed_epoch - retire_epoch >= LJ_TAB_RETIRE_EPOCHS;
}

static int tab_node_still_published(global_State *g, const TabNodeRetire *ret)
{
  GCtab *t = lj_tab_node_retired_tab_acq(ret);
  if (!t)
    return 0;
  return lj_gc2_tab_generation_current(
    g, t, lj_tab_node_retired_node_acq(ret), 0);
}

static int tab_array_still_published(global_State *g, const TabArrayRetire *ret)
{
  GCtab *t = lj_tab_array_retired_tab_acq(ret);
  if (!t)
    return 0;
  return lj_gc2_tab_generation_current(
    g, t, lj_tab_array_retired_array_acq(ret), 1);
}

/* Retire records and their vector allocations are separate arena objects. The
** exclusive list ticket proves the record, but it does not by itself make a
** stale vector-header dereference legal. Validate the nested allocation with
** the exact-thread reclaim certificate and retain the record on any mismatch.
*/
static int tab_node_reclaim_body_valid(global_State *g,
				       const TabNodeRetire *ret,
				       Node **nodep, MSize *hmaskp)
{
  Node *node = lj_tab_node_retired_node_acq(ret);
  MSize hmask = lj_tab_node_retired_hmask_acq(ret);
  if (!node || node == &g->nilnode ||
      !lj_gc2_mem_registered_known_reclaim_held(
	g, (const void *)lj_tab_node_hdr(node)) ||
      !tab_node_free_ready(node, hmask))
    return 0;
  *nodep = node;
  *hmaskp = hmask;
  return 1;
}

static int tab_array_reclaim_body_valid(global_State *g,
					 const TabArrayRetire *ret,
					 TValue **arrayp, MSize *acapp)
{
  TValue *array = lj_tab_array_retired_array_acq(ret);
  MSize acap = lj_tab_array_retired_acap_acq(ret);
  if (!array ||
      !lj_gc2_mem_registered_known_reclaim_held(
	g, (const void *)lj_tab_array_hdr(array)) ||
      !tab_array_free_ready(array, acap))
    return 0;
  *arrayp = array;
  *acapp = acap;
  return 1;
}

static LJ_AINLINE void tab_read_oldest_on_tg(global_State *g, TGState *tg,
					      uint64_t *oldestp)
{
  if (tg && tg->gl == g && lj_tg_tab_read_depth_acq(tg) != 0) {
    uint64_t epoch = lj_tg_tab_read_epoch_acq(tg);
    if (epoch < *oldestp)
      *oldestp = epoch;
  }
}

/* Return the oldest outer table-vector read which can still name a generation
** retired in that epoch. The legacy TG list is append-only until the separate
** SMR writer gate removes dead nodes; ordinary reclaim already owns that gate.
** Main/self fallbacks cover attach/detach-adjacent owner publications which are
** temporarily absent from the list, matching the string-table pin contract. */
static uint64_t tab_read_oldest_epoch(global_State *g)
{
  TGState *tg, *main_tg, *self;
  uint64_t oldest = ~(uint64_t)0;
  int saw_main = 0, saw_self = 0;
  main_tg = g->main_tg;
  self = lj_thr_get_tg();
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg)) {
    if (tg == main_tg)
      saw_main = 1;
    if (tg == self)
      saw_self = 1;
    tab_read_oldest_on_tg(g, tg, &oldest);
  }
  if (!saw_main)
    tab_read_oldest_on_tg(g, main_tg, &oldest);
  if (!saw_self && self != main_tg)
    tab_read_oldest_on_tg(g, self, &oldest);
  return oldest;
}

/*
** Q: Why all of these copies of t->hmask, t->node etc. to local variables?
** A: Because alias analysis for C is _really_ tough.
**    Even state-of-the-art C compilers won't produce good code without this.
*/

static LJ_AINLINE void clearhpart_node(Node *node, MSize hmask)
{
  MSize i;
  lj_assertX(hmask != 0, "empty hash part");
  for (i = 0; i <= hmask; i++) {
    Node *n = &node[i];
    lj_tab_nextnode_rel(n, NULL);
    lj_tab_storenilraw(&n->val);
    lj_tab_storenilraw(&n->key);
  }
}

/* Clear hash part of table. */
static LJ_AINLINE void clearhpart(GCtab *t)
{
  MSize hmask;
  Node *node = lj_tab_node_snapshot_acq(t, &hmask);
  clearhpart_node(node, hmask);
}

/* Clear array part of table. */
static LJ_AINLINE void cleararray(TValue *array, uint32_t asize)
{
  uint32_t i;
  for (i = 0; i < asize; i++)
    lj_tab_storenilraw(&array[i]);
}

static LJ_AINLINE void clearapart(GCtab *t)
{
  TValue *array;
  uint32_t asize = (uint32_t)lj_tab_array_snapshot_acq(t, &array);
  cleararray(array, asize);
}

static LJ_AINLINE void tab_init_empty(global_State *g, GCtab *t)
{
  Node *nilnode = &g->nilnode;
  t->gct = ~LJ_TTAB;
  lj_tab_nomm_rel(t, (uint8_t)~0);
  lj_tab_colo_rel(t, 0);
  lj_tab_array_set(t, NULL);
  lj_tab_metatable_rel(t, NULL);
  lj_tab_asize_rel(t, 0);
  lj_tab_acap_rel(t, 0);
  lj_tab_hmask_rel(t, 0);
  lj_tab_node_set(t, nilnode);
  lj_tab_struct_owner_store_rlx(t, 0);
  lj_tab_weak_cycle_store_rlx(t, 0);
  lj_tab_gc2_rescan_state_store_rlx(t, LJ_TAB_RESCAN_NONE);
  lj_tab_freetop_rel(t, nilnode);
}

static LJ_AINLINE void tab_publish_new(lua_State *L, GCtab *t,
				       TValue *anchor)
{
  global_State *g = G(L);
  TValue tv;
  newwhite(g, t);
  settabV(L, &tv, t);
  copyTVrel(L, anchor, &tv);  /* Semantic root precedes READY. */
  lj_gc_publishobj_header(g, obj2gco(t));
  lj_gc_pubroot(L, anchor);
  /* Test-only collections are valid from this point onward: the complete
  ** table is READY and its semantic construction root has been barriered.
  ** Production constructors never poll/ACK/yield in the preceding READY=0
  ** interval, where the nil anchor deliberately does not retain side bodies. */
  tab_test_constructor_prepublish(L, t);
  lj_gc_linkobj_new(g, obj2gco(t));  /* Publish table after body init. */
}

static LJ_AINLINE void tab_publish_array(GCtab *t, TValue *array,
					 uint32_t asize, uint32_t acap)
{
  lj_tab_acap_rel(t, acap);
  lj_tab_array_rel(t, array);
  lj_tab_asize_rel(t, asize);
}

LJ_STATIC_ASSERT(((sizeof(GCtab) + LJ_CELL_SIZE-1u) >> LJ_CELL_SHIFT) == 5u);

/* Create a new table with slots initialized to nil. */
static GCtab *newtab_rooted(lua_State *L, uint32_t asize, uint32_t hbits,
			    LJTabRoot *root)
{
  global_State *g = G(L);
  GCtab *t = NULL;
  TValue *array = NULL;
  Node *node = NULL;
  TGState *tg = L2TG(L);
  TValue nilv;
  TValue *anchor;
  uint32_t anchoridx;
  MSize hmask = 0;
  GCSize tbytes, abytes = 0, nbytes = 0;
  int colocated = LJ_MAX_COLOSIZE != 0 && asize > 0 &&
		  asize <= LJ_MAX_COLOSIZE;
  if (root) {
    root->tg = NULL;
    root->idx = 0;
  }
  if (asize > LJ_MAX_ASIZE || hbits > LJ_MAX_HBITS)
    lj_err_msg(L, LJ_ERR_TABOV);
  lj_assertL((sizeof(GCtab) & 7) == 0, "bad GCtab size");
  tbytes = (GCSize)(colocated ? sizetabcolo(asize) : sizeof(GCtab));
  setnilV(&nilv);
  anchor = lj_tg_root_anchor_push(L, tg, &nilv, &anchoridx);
  if (!anchor)
    lj_err_mem(L);

  /* Allocate all fallible side bodies before the header. Their accounting is
  ** deferred, so no GC assistance can run between allocation and the final
  ** table publication. This avoids both a READY table with half-built roots
  ** and a leaked READY=0 header after a later OOM. */
  if (!colocated && asize > 0) {
    abytes = (GCSize)lj_tab_array_bytes(asize);
    array = tab_array_new_deferred_nothrow(L, asize, asize);
    if (!array)
      goto oom;
    cleararray(array, asize);
  }
  if (hbits) {
    node = newhpart_alloc_deferred_nothrow(L, hbits, &hmask);
    if (!node)
      goto oom;
    nbytes = (GCSize)lj_tab_node_bytes(hmask);
  }
  t = (GCtab *)lj_mem_newgco_unlinked_deferred_nothrow(L, tbytes);
  if (!t)
    goto oom;
  tab_init_empty(g, t);
  if (colocated) {
    array = (TValue *)((char *)t + sizeof(GCtab));
    lj_tab_colo_rel(t, (int8_t)asize);
    cleararray(array, asize);
    tab_publish_array(t, array, asize, asize);
  } else if (array) {
    tab_pub_array_mem(L, t, array, asize);
    tab_publish_array(t, array, asize, asize);
  }
  if (node)
    newhpart_publish(L, t, node, hmask, &node[hmask+1], hmask + 1u);

  /* READY and ownership are the single final constructor publication. Every
  ** array/hash root and raw side-body mark is already visible at this point. */
  tab_publish_new(L, t, anchor);
  if (abytes)
    lj_mem_account_deferred(L, abytes);
  if (nbytes)
    lj_mem_account_deferred(L, nbytes);
  lj_mem_account_deferred(L, tbytes);
  if (root) {
    root->tg = tg;
    root->idx = anchoridx;
  } else {
    lj_tg_root_anchor_pop(tg, anchoridx);
  }
  return t;

oom:
  if (t)
    lj_mem_freegco_unpublished(g, t, tbytes);
  if (node)
    lj_mem_free(g, lj_tab_node_hdrw(node), lj_tab_node_bytes(hmask));
  if (array)
    lj_mem_free(g, lj_tab_array_hdrw(array), lj_tab_array_bytes(asize));
  lj_tg_root_anchor_pop(tg, anchoridx);
  lj_err_mem(L);
  return NULL;  /* Unreachable. */
}

static GCtab *newtab(lua_State *L, uint32_t asize, uint32_t hbits)
{
  return newtab_rooted(L, asize, hbits, NULL);
}

/* Create a new table.
**
** IMPORTANT NOTE: The API differs from lua_createtable()!
**
** The array size is non-inclusive. E.g. asize=128 creates array slots
** for 0..127, but not for 128. If you need slots 1..128, pass asize=129
** (slot 0 is wasted in this case).
**
** The hash size is given in hash bits. hbits=0 means no hash part.
** hbits=1 creates 2 hash slots, hbits=2 creates 4 hash slots and so on.
*/
GCtab *lj_tab_new(lua_State *L, uint32_t asize, uint32_t hbits)
{
  if (asize == 0 && hbits == 0)
    return lj_tab_new0(L);
  return newtab(L, asize, hbits);
}

/*
** Empty-table bump allocation is only valid for the single-producer main-TG
** window. Otherwise use newtab(), which owns allocator fallback, MT
** publication, worker, and non-empty-table construction. Generic allocation
** still prefers reusable free-run bins. This leaf empty-table specialization may
** consume the active bump window first: Lua does not expose table address reuse
** order, and the active bump window is never present in the reusable-bin lists.
*/
static GCtab *tab_new0_bump(lua_State *L, global_State *g, TGState *tg)
{
  const uint32_t ncells = lj_arena_ncells(sizeof(GCtab));
  LJArenaBump *b;
  GCArena *a;
  GCtab *t;
  uint64_t local_total;
  uint32_t cell, end, next;
  int account_now, black;
  UNUSED(L);
  if (g == NULL || tg == NULL ||
      mt_active_or_entering_acq(g) || gc2_n_workers_acq(g) != 0 ||
      g->allocf_arena == 0 || tg != g->main_tg ||
      lj_tg_flags_test_acq(tg, TGF_DEAD) ||
      !lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL) ||
      g->allocd != &tg->allocd)
    return NULL;
  local_total = lj_tg_local_total_acq(tg);
  account_now = local_total >= LJ_GC2_ACCT_FLUSH - sizeof(GCtab);
  /* A bump result has no caller-visible root until this helper returns. The
  ** rare accounting-flush case can assist a new cycle, so route it through
  ** anchored newtab() instead of polling after READY with only a C local. */
  if (account_now)
    return NULL;
  b = &tg->alloc.bump[LJ_ARENAK_TRAVERSABLE];
  a = b->a;
  if (a == NULL)
    return NULL;
  cell = b->cell;
  end = b->end;
  next = cell + ncells;
  if (next < cell || next > end)
    return NULL;
  /* Claim construction ownership before either consuming the private window
  ** or exposing header bytes. A failed claim leaves the bump cursor intact. */
  if (LJ_UNLIKELY(!lj_arena_root_construct_claim(a, cell)))
    return NULL;
  b->cell = next;
  t = (GCtab *)lj_arena_cellptr(a, cell);
  tab_init_empty(g, t);
  newwhite(g, t);
  black = lj_arena_alloc_black_acq(&tg->alloc);
  if (black)
    lj_arena_bm_set(a->mark, cell);
  else
    lj_arena_bm_clear(a->mark, cell);
  lj_arena_ready_set_unpublished(a, cell);
  /* block[] is the discovery publication: expose only a complete header. */
  lj_arena_block_set(a, cell);
  lj_gc_total_add(g, sizeof(GCtab));
  /*
  ** The arena block bit is visible to bitmap sweep before this helper returns.
  ** Publish the initialized table, or prove GC2 arena ownership, before an
  ** accounting flush can assist GC.
  */
  /*
  ** A birth mark protects the body but does not describe the object occupying
  ** the arena run. Always publish the exact table header through the per-TG
  ** pending ownership chain so GC2 can prune and destruct it during sweep.
  */
  lj_gc_linkobj_new(g, obj2gco(t));
  (void)lj_tg_local_total_add_rlx(tg, sizeof(GCtab));
  return t;
}

GCtab * LJ_FASTCALL lj_tab_new0(lua_State *L)
{
  GCtab *t;
  tab_test_new0_call();
  t = tab_new0_bump(L, G(L), L2TG(L));
  return t ? t : newtab(L, 0, 0);
}

/* The API of this function conforms to lua_createtable(). */
GCtab *lj_tab_new_ah(lua_State *L, uint32_t a, uint32_t h)
{
  if (a == 0 && h == 0)
    return lj_tab_new0(L);
  return lj_tab_new(L, a ? a+1 : 0, hsize2hbits(h));
}

GCtab *lj_tab_new_rooted(lua_State *L, uint32_t asize, uint32_t hbits,
			  LJTabRoot *root)
{
  lj_assertL(root != NULL, "missing table construction root");
  /* Rooted callers deliberately bypass the empty bump specialization: the TG
  ** anchor, not a transient birth mark, spans their semantic handoff. */
  return newtab_rooted(L, asize, hbits, root);
}

GCtab *lj_tab_new_ah_rooted(lua_State *L, uint32_t a, uint32_t h,
			     LJTabRoot *root)
{
  /* Deliberately bypass the empty-table bump specialization. The rooted API
  ** is for callers which may poll or wait before installing the ordinary Lua
  ** root, so the pre-reserved TG anchor must span the entire handoff. */
  return lj_tab_new_rooted(L, a ? a+1 : 0, hsize2hbits(h), root);
}

void lj_tab_root_release(LJTabRoot *root)
{
  if (!root || !root->tg)
    return;
  lj_tg_root_anchor_pop(root->tg, root->idx);
  root->tg = NULL;
  root->idx = 0;
}

#if LJ_HASJIT
GCtab * LJ_FASTCALL lj_tab_new0_forjit(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  GCtab *t = tab_new0_bump(L, g, tg);
  return t ? t : lj_tab_new0(L);
}

GCtab * LJ_FASTCALL lj_tab_new1(lua_State *L, uint32_t ahsize)
{
  return newtab(L, ahsize & 0xffffff, ahsize >> 24);
}
#endif

/* Duplicate a table. */
GCtab * LJ_FASTCALL lj_tab_dup(lua_State *L, const GCtab *kt)
{
  GCtab *t;
  TValue *array, *karray;
  Node *node, *knode;
  uint32_t asize, hmask;
  MSize khmask, thmask, tasize;
  knode = lj_tab_node_snapshot_acq(kt, &khmask);
  hmask = (uint32_t)khmask;
  asize = (uint32_t)lj_tab_array_snapshot_acq(kt, &karray);
  t = newtab(L, asize, hmask > 0 ? lj_fls(hmask)+1 : 0);
  knode = lj_tab_node_snapshot_acq(kt, &khmask);
  asize = (uint32_t)lj_tab_array_snapshot_acq(kt, &karray);
  tasize = lj_tab_array_snapshot_acq(t, &array);
  node = lj_tab_node_snapshot_acq(t, &thmask);
  UNUSED(tasize);
  lj_assertL(hmask == (uint32_t)khmask &&
	     asize == (uint32_t)tasize &&
	     hmask == (uint32_t)thmask,
	     "mismatched size of table and template");
  lj_tab_nomm_rel(t, 0);  /* Keys with metamethod names may be present. */
  if (asize > 0) {
    uint32_t i;
    for (i = 0; i < asize; i++)
      lj_tv_load_acq(&array[i], &karray[i]);
  }
  if (hmask > 0) {
    uint32_t i;
    ptrdiff_t d = (char *)node - (char *)knode;
    MSize freecount = 0;
    setfreetop(t, node, (Node *)((char *)getfreetop(kt, knode) + d));
    for (i = 0; i <= hmask; i++) {
      Node *kn = &knode[i];
      Node *n = &node[i];
      TValue key, val;
      Node *next = lj_tab_nextnode_acq(kn);
      /* Don't use copyTV here, since it asserts on a copy of a dead key. */
      lj_tv_load_acq(&val, &kn->val);
      lj_tv_load_acq(&key, &kn->key);
      /* Private duplicate table construction; not shared publication. */
      n->val = val; n->key = key;
      if (tvistab(&val)) {  /* Replace nil value marker. */
	lj_tab_storenilraw(&n->val);
	setnilV(&val);
      }
      if (tvisnil(&key) && tvisnil(&val))
	freecount++;
      lj_tab_nextnode_set(n, next == NULL ? next : (Node *)((char *)next + d));
    }
    lj_tab_node_freecount_set_rel(node, freecount);
  }
  /*
  ** newtab() publishes the header before the template payload is copied. An
  ** active-black duplicate can therefore be marked before its child edges are
  ** visible; publish the completed table so GC2 rescans the copied payload.
  */
  lj_gc_pubtab(L, t);
  return t;
}

/* Clear a private table. */
static void tab_clear_raw(GCtab *t)
{
  Node *node;
  MSize hmask;
  clearapart(t);
  node = lj_tab_node_snapshot_acq(t, &hmask);
  if (hmask > 0) {
    setfreetop(t, node, &node[hmask+1]);
    clearhpart_node(node, hmask);
    lj_tab_node_freecount_set_rel(node, hmask+1);
  }
}

/* Free a table. */
void LJ_FASTCALL lj_tab_free(global_State *g, GCtab *t)
{
  MSize colosz = lj_tab_colo_size(t);
  MSize size = LJ_MAX_COLOSIZE != 0 && colosz ?
	       sizetabcolo(colosz) : sizeof(GCtab);
  MSize hmask;
  Node *node = lj_tab_node_snapshot_acq(t, &hmask);
  TValue *array;
  MSize acap = lj_tab_array_separated_snapshot_acq(t, &array);
  if (hmask > 0)
    tab_node_free(g, node, hmask);
  if (acap > 0)
    tab_array_free(g, array, acap);
  if (!lj_mem_freegco_defer(g, t, size))
    lj_mem_free(g, t, size);
}

/* -- Table resizing ------------------------------------------------------ */

#ifdef LJ_TAB_TEST_HELPERS
static LJTabResizeArrayHook tab_test_resize_colocated_after_freeze_hook;

static LJ_AINLINE void tab_test_resize_colocated_after_freeze(lua_State *L,
							      GCtab *t,
							      TValue *oldarray,
							      MSize oldasize)
{
  if (tab_test_resize_colocated_after_freeze_hook)
    tab_test_resize_colocated_after_freeze_hook(L, t, oldarray, oldasize);
}
#else
#define tab_test_resize_colocated_after_freeze(L, t, oldarray, oldasize) \
  ((void)(L), (void)(t), (void)(oldarray), (void)(oldasize))
#endif

/* Resize a table to fit the new array/hash part sizes. */
void lj_tab_resize(lua_State *L, GCtab *t, uint32_t asize, uint32_t hbits)
{
  global_State *g = G(L);
  MSize oldhmask;
  Node *oldnode;
  TValue *oldarray;
  uint32_t oldasize;
  int oldarray_separated;
  uint32_t oldacap;
  TValue *array;
  uint32_t newacap;
  int array_changed;
  int newarray;
  MSize newhmask;
  Node *newnode;
  Node *newfreetop;
  MSize newfreecount;
  uint32_t hashcount;
  MSize target_hmask;
  uint32_t hash_flags0;
  TabNodeRetire *oldret;
  TabArrayRetire *oldaret;
  int array_next_claimed;
  int struct_acq;
  Node *node_succ;
  int deadkey;
  int count_retry;
  int weak;
  int shared_resize;

restart_resize:
  oldnode = lj_tab_node_snapshot_acq(t, &oldhmask);
  oldasize = (uint32_t)lj_tab_array_snapshot_acq(t, &oldarray);
  oldarray_separated = oldarray && !lj_tab_array_is_colocated(t, oldarray);
  oldacap = oldarray_separated ?
    (uint32_t)lj_tab_array_hdr_acap_acq(oldarray) : oldasize;
  /*
  ** Resize uses old vector metadata both for bounded migration loops and for
  ** delayed physical free. A concurrent generation hand-off may leave a mutator
  ** with an old snapshot; consume it only while the side-vector headers still
  ** describe valid LuaJIT table allocations.
  */
  if ((oldhmask > 0 && !tab_node_free_ready(oldnode, oldhmask)) ||
      oldasize > LJ_MAX_ASIZE ||
      (oldarray_separated &&
       (!tab_array_free_ready(oldarray, oldacap) || oldasize > oldacap))) {
    lj_tab_wait_l(L);
    goto restart_resize;
  }
  array = oldarray;
  newacap = oldacap;
  array_changed = asize != oldasize;
  newarray = 0;
  newhmask = 0;
  newnode = NULL;
  newfreetop = NULL;
  newfreecount = 0;
  oldret = NULL;
  oldaret = NULL;
  array_next_claimed = 0;
  struct_acq = 0;
  node_succ = NULL;
  deadkey = 0;
  count_retry = 0;
  weak = lj_gc2_weak_write_candidate(L, t);
  shared_resize = !tab_private_mutation_allowed(L);
  hash_flags0 = oldhmask > 0 ? lj_tab_node_hdr_flags_word_acq(oldnode) : 0;
  if (oldhmask > 0 && (hash_flags0 & (uint32_t)TABNODE_FLAG_RETIRING)) {
    lj_tab_wait_l(L);
    goto restart_resize;
  }
  hashcount = tab_rehash_hashcount(L, oldnode, oldhmask, oldasize, asize,
				   &deadkey, &count_retry);
  if (count_retry) {
    /* No raw generation pointer survives the safepoint-capable wait. */
    lj_tab_wait_l(L);
    goto restart_resize;
  }
  if (hashcount) {
    uint32_t needhbits = hsize2hbits(hashcount);
    if (hbits < needhbits)
      hbits = needhbits;
  }
  if (shared_resize && oldhmask > 0) {
    uint32_t oldhbits = lj_fls((uint32_t)oldhmask) + 1u;
    /*
    ** Shared writers can insert into the current hash generation after this
    ** sizing pass and before the owner publishes TABNODE_FLAG_RETIRING. Keep at
    ** least the old hash capacity for shared resizes, so those late inserts have
    ** a destination during migration. Private single-mutator resizes retain the
    ** stock shrink-to-fit behavior.
    */
    if (hbits < oldhbits)
      hbits = oldhbits;
  }
  if (hbits > LJ_MAX_HBITS)
    lj_err_msg(L, LJ_ERR_TABOV);
  target_hmask = hbits ? (((MSize)1u << hbits) - 1u) : 0;
  if (asize > oldasize && asize > LJ_MAX_ASIZE)
    lj_err_msg(L, LJ_ERR_TABOV);
  if (array_changed) {
    if (oldarray_separated) {
      newarray = 1;
      newacap = asize >= oldasize ? asize : oldacap;
    } else if (asize > oldacap) {
      newarray = 1;
      newacap = asize;
    }
  }
  /*
  ** A resize request can be redundant after another mutator has already
  ** published the requested generation. Still rebuild same-sized hash parts
  ** when dead value slots are present: that compaction is observable as future
  ** insert capacity and cannot be skipped safely.
  */
  if (!array_changed && oldhmask == target_hmask && !deadkey)
    return;
  /*
  ** Another mutator may publish the requested generation after the snapshot
  ** above has been sized and counted. Recheck before allocating replacement
  ** storage; the owner-side recheck below remains the authoritative guard for
  ** the publication window.
  */
  {
    TValue *curarray;
    MSize curasize, curhmask;
    Node *curnode = lj_tab_node_snapshot_acq(t, &curhmask);
    curasize = lj_tab_array_snapshot_acq(t, &curarray);
    if (curnode != oldnode || curhmask != oldhmask ||
	curarray != oldarray || (uint32_t)curasize != oldasize) {
      lj_tab_wait_l(L);
      goto restart_resize;
    }
  }
  if (newarray) {
    uint32_t i;
    array = tab_array_new(L, asize, newacap);
    tab_pub_array_mem(L, t, array, newacap);
    for (i = 0; i < newacap; i++)
      lj_tab_storenilraw(&array[i]);
  }
  if (hbits) {
    newnode = newhpart_alloc(L, hbits, &newhmask);
    tab_pub_node_mem(L, t, newnode);
    newfreetop = &newnode[newhmask+1];
    newfreecount = newhmask + 1u;
  }
  if (oldhmask > 0)
    oldret = tab_retire_reserve(L);
  if (newarray && oldarray_separated && oldacap > 0)
    oldaret = tab_array_retire_reserve(L);
  /* Replacement storage is already owned by this attempt. A STOPREQ throw
  ** while waiting for the structural slot would leak it, so this retry window
  ** uses the TLS owner and yield-only wait variant. No old vector is
  ** dereferenced until the root recheck below succeeds. */
  struct_acq = shared_resize ? lj_tab_struct_enter(NULL, t) : 0;
  /*
  ** From here until lj_tab_struct_leave(), retry waits must not raise a fresh
  ** STOPREQ: a longjmp would leak the per-table structural owner and the
  ** replacement arrays/nodes reserved for this attempt. Use yield-only waits
  ** inside the critical publication window; the retry path below releases all
  ** claims and then performs the L-aware STOPREQ-visible wait.
  */
  {
    TValue *curarray;
    MSize curasize = lj_tab_array_snapshot_acq(t, &curarray);
    if (curarray != oldarray || (uint32_t)curasize != oldasize ||
	lj_tab_node_acq(t) != oldnode)
      goto retry_resize;
    if (!shared_resize && !tab_private_mutation_allowed(L))
      goto retry_resize;
  }
  if (oldret)
    tab_retire_init(L, oldret, t, oldnode, oldhmask);
  if (oldaret)
    tab_array_retire_init(L, oldaret, t, oldarray, oldacap);
  if (asize > oldasize) {  /* Array part grows? */
    uint32_t i;
    for (i = oldasize; i < asize; i++)  /* Clear newly allocated/current slots. */
      lj_tab_storenilraw(&array[i]);
  }
  if (oldaret) {
    const TValue *expect = NULL;
    if (!lj_tab_array_nextgen_cas(oldarray, &expect, array))
      goto retry_resize;
    array_next_claimed = 1;
  }
  if (oldret) {
    const Node *expect_next = NULL;
    uint32_t expect_flags = hash_flags0;
    uint32_t want_flags = hash_flags0 | (uint32_t)TABNODE_FLAG_RETIRING;
    node_succ = hbits ? newnode : &g->nilnode;
    if (!lj_tab_node_nextgen_cas(oldnode, &expect_next, node_succ))
      goto retry_resize;
    if (!lj_tab_node_hdr_flags_word_cas(oldnode, &expect_flags, want_flags)) {
      const Node *revert = node_succ;
      (void)lj_tab_node_nextgen_cas(oldnode, &revert, NULL);
      goto retry_resize;
    }
    tab_retired_push(g, oldret);
  }
  if (oldaret) {
    lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING);
    tab_array_retired_push(g, oldaret);
  }
  if (newarray && !oldarray_separated && oldarray) {
    uint32_t i;
    uint32_t copy = oldasize < newacap ? oldasize : newacap;
    for (i = 0; i < copy; i++)
	  (void)tab_resize_copy_array_slot(L, t, oldarray, i, array, asize,
					       newnode, newhmask, &newfreetop,
					       &newfreecount, 1, weak);
    tab_test_resize_colocated_after_freeze(L, t, oldarray, oldasize);
  }
  if (newarray && oldarray_separated) {
    uint32_t i;
    for (i = 0; i < oldasize; i++)
	  (void)tab_resize_copy_array_slot(L, t, oldarray, i, array, asize,
					       newnode, newhmask, &newfreetop,
					       &newfreecount, 0, weak);
  }
  if (oldhmask > 0) {  /* Reinsert pairs from old hash part. */
    uint32_t i;
    for (i = 0; i <= oldhmask; i++)
	      (void)tab_resize_copy_hash_slot(L, t, oldnode, oldhmask, i, array,
					      asize, newnode, newhmask, &newfreetop,
					      &newfreecount, oldret != NULL,
					      weak);
  }
  if (!oldarray_separated && oldarray && asize < oldasize) {
    /* Freeze and reinsert old colocated array tail off-table. */
    uint32_t i;
    for (i = asize; i < oldasize; i++)
	  (void)tab_resize_copy_array_slot(L, t, oldarray, i, array, asize,
					       newnode, newhmask, &newfreetop,
					       &newfreecount, 1, weak);
    tab_test_resize_colocated_after_freeze(L, t, oldarray, oldasize);
  }
  if (newarray) {
    if (LJ_MAX_COLOSIZE != 0) {
      int8_t colo = lj_tab_colo_acq(t);
      if (colo > 0)
	lj_tab_colo_rel(t, (int8_t)(colo | 0x80));  /* Mark separated. */
    }
    lj_tab_acap_rel(t, newacap);
  }
  if (asize > oldasize) {
    if (array != oldarray)
      lj_tab_array_rel(t, array);
    lj_tab_asize_rel(t, asize);
    if (oldaret)
      tab_array_retire_arm(G(L), oldaret);
  }
  /* Publish the rebuilt hash part. */
  if (hbits) {
    newhpart_publish(L, t, newnode, newhmask, newfreetop, newfreecount);
    if (oldret)
      tab_retire_arm(G(L), oldret);
  } else {
    global_State *g = G(L);
    lj_tab_hmask_rel(t, 0);
    lj_tab_freetop_rel(t, &g->nilnode);
    lj_tab_node_rel(t, &g->nilnode);
    if (oldret) {
      tab_retire_arm(g, oldret);
    }
  }
  if (asize < oldasize) {  /* Array part shrinks? */
    lj_tab_asize_rel(t, asize);  /* This 'shrinks' even colocated arrays. */
    if (array != oldarray)
      lj_tab_array_rel(t, array);
    if (oldaret)
      tab_array_retire_arm(G(L), oldaret);
  }
  if (oldhmask > 0)
    lj_assertL(oldret && la_load32_acq(&oldret->armed),
	       "retired table nodes not armed");
  lj_tab_struct_leave(t, struct_acq);
  return;

retry_resize:
  if (array_next_claimed) {
    const TValue *revert = array;
    (void)lj_tab_array_nextgen_cas(oldarray, &revert, NULL);
  }
  if (newnode)
    tab_node_free(g, newnode, newhmask);
  if (newarray && array && array != oldarray)
    tab_array_free(g, array, newacap);
  tab_retire_discard(g, oldret);
  tab_array_retire_discard(g, oldaret);
  lj_tab_struct_leave(t, struct_acq);
  lj_tab_wait_l(L);
  goto restart_resize;
}

static LJ_AINLINE int tab_retire_record_known(global_State *g,
					       const void *p,
					       int reclaim_held)
{
  return reclaim_held ?
    lj_gc2_mem_registered_known_reclaim_held(g, p) :
    lj_gc2_mem_registered_known(g, p);
}

/* Preflight detached lists before clearing a link or freeing a record. This
** catches both cycles and duplicate pushes without truncating a suffix or
** revisiting a record after its first free. */
#define TAB_RETIRE_CHAIN_VALIDATOR(name, type, nextfn) \
static int name(global_State *g, type *head, int reclaim_held) \
{ \
  type *slow = head, *fast = head; \
  while (fast) { \
    if (slow) { \
      if (!tab_retire_record_known(g, slow, reclaim_held)) return 0; \
      slow = nextfn(slow); \
    } \
    if (!tab_retire_record_known(g, fast, reclaim_held)) return 0; \
    fast = nextfn(fast); \
    if (fast) { \
      if (!tab_retire_record_known(g, fast, reclaim_held)) return 0; \
      fast = nextfn(fast); \
    } \
    if (fast && slow == fast) return 0; \
  } \
  return 1; \
}

TAB_RETIRE_CHAIN_VALIDATOR(tab_node_retired_chain_valid, TabNodeRetire,
			   lj_tab_node_retired_next_acq)
TAB_RETIRE_CHAIN_VALIDATOR(tab_array_retired_chain_valid, TabArrayRetire,
			   lj_tab_array_retired_next_acq)

#undef TAB_RETIRE_CHAIN_VALIDATOR

uint32_t lj_tab_reclaim_retired(global_State *g, uint64_t completed_epoch)
{
  TabNodeRetire *ret;
  TabArrayRetire *aret;
  uint64_t oldest_reader;
  uint32_t reclaimed = 0;
  if (!g || completed_epoch == 0)
    return 0;
  /*
  ** Multi-TG reclamation is valid only inside GC2's exact-thread exclusive-
  ** reclaimer scope. It excludes competing retired-list consumers/TG body
  ** reclamation but never waits for a reader. Long C scans publish a per-TG
  ** outer epoch; a pin old enough to name this generation simply makes the
  ** record get requeued.
  ** Readers which start after retirement acquire the replacement root and have
  ** a newer epoch, so they do not delay old storage. The table-published root
  ** check remains a separate cold-path validation before physical free.
  */
  oldest_reader = tab_read_oldest_epoch(g);
  /*
  ** Flush pending table roots once before the conservative published-root
  ** checks below. Each retired generation still gets its own root-list scan,
  ** but repeated pending-root drains are unnecessary and can be expensive when
  ** reclaiming a batch of old table generations.
  */
  (void)lj_gc_flush_root_pending(g);
  ret = lj_tab_node_retired_head_xchg_acqrel(g, NULL);
  if (LJ_UNLIKELY(!tab_node_retired_chain_valid(g, ret, 1))) {
    lj_assertG(0, "invalid/cyclic detached table-node retire chain");
    abort();
  }
  while (ret) {
    Node *node = NULL;
    MSize hmask = 0;
    int current;
    if (LJ_UNLIKELY(
	!lj_gc2_mem_registered_known_reclaim_held(g, ret))) {
      lj_assertG(0, "invalid detached table-node retire record");
      abort();
    }
    TabNodeRetire *next = lj_tab_node_retired_next_acq(ret);
    lj_tab_node_retired_next_rel(ret, NULL);
    current = tab_node_still_published(g, ret);
    if (!lj_tab_node_retired_armed_acq(ret)) {
      tab_retired_push(g, ret);
    } else if (tab_retire_epoch_elapsed(completed_epoch,
					lj_tab_node_retired_epoch_acq(ret)) &&
		       oldest_reader > lj_tab_node_retired_epoch_acq(ret) &&
		       current == 0 &&
		       tab_node_reclaim_body_valid(g, ret, &node, &hmask)) {
      tab_node_free(g, node, hmask);
      lj_mem_freet(g, ret);
      reclaimed++;
    } else {
      tab_retired_push(g, ret);
    }
    ret = next;
  }
  aret = lj_tab_array_retired_head_xchg_acqrel(g, NULL);
  if (LJ_UNLIKELY(!tab_array_retired_chain_valid(g, aret, 1))) {
    lj_assertG(0, "invalid/cyclic detached table-array retire chain");
    abort();
  }
  while (aret) {
    TValue *array = NULL;
    MSize acap = 0;
    int current;
    if (LJ_UNLIKELY(
	!lj_gc2_mem_registered_known_reclaim_held(g, aret))) {
      lj_assertG(0, "invalid detached table-array retire record");
      abort();
    }
    TabArrayRetire *next = lj_tab_array_retired_next_acq(aret);
    lj_tab_array_retired_next_rel(aret, NULL);
    current = tab_array_still_published(g, aret);
    if (!lj_tab_array_retired_armed_acq(aret)) {
      tab_array_retired_push(g, aret);
    } else if (tab_retire_epoch_elapsed(completed_epoch,
					lj_tab_array_retired_epoch_acq(aret)) &&
		       oldest_reader > lj_tab_array_retired_epoch_acq(aret) &&
		       current == 0 &&
		       tab_array_reclaim_body_valid(g, aret, &array, &acap)) {
      tab_array_free(g, array, acap);
      lj_mem_freet(g, aret);
      reclaimed++;
    } else {
      tab_array_retired_push(g, aret);
    }
    aret = next;
  }
  return reclaimed;
}

void lj_tab_freeretired(global_State *g)
{
  TabNodeRetire *ret;
  TabArrayRetire *aret;
  if (!g)
    return;
  ret = lj_tab_node_retired_head_xchg_acqrel(g, NULL);
  if (LJ_UNLIKELY(!tab_node_retired_chain_valid(g, ret, 0))) {
    lj_assertG(0, "invalid/cyclic terminal table-node retire chain");
    abort();
  }
  while (ret) {
    Node *node;
    MSize hmask;
    if (LJ_UNLIKELY(!lj_gc2_mem_registered_known(g, ret))) {
      lj_assertG(0, "invalid terminal table-node retire record");
      abort();
    }
    TabNodeRetire *next = lj_tab_node_retired_next_acq(ret);
    node = lj_tab_node_retired_node_acq(ret);
    hmask = lj_tab_node_retired_hmask_acq(ret);
    if (lj_tab_node_retired_armed_acq(ret)) {
      if (LJ_UNLIKELY(!node || node == &g->nilnode ||
	  !lj_gc2_mem_registered_known(g, lj_tab_node_hdr(node)) ||
	  !tab_node_free_ready(node, hmask))) {
	lj_assertG(0, "invalid terminal retired table-node body");
	abort();
      }
      tab_node_free(g, node, hmask);
    }
    lj_mem_freet(g, ret);
    ret = next;
  }
  aret = lj_tab_array_retired_head_xchg_acqrel(g, NULL);
  if (LJ_UNLIKELY(!tab_array_retired_chain_valid(g, aret, 0))) {
    lj_assertG(0, "invalid/cyclic terminal table-array retire chain");
    abort();
  }
  while (aret) {
    TValue *array;
    MSize acap;
    if (LJ_UNLIKELY(!lj_gc2_mem_registered_known(g, aret))) {
      lj_assertG(0, "invalid terminal table-array retire record");
      abort();
    }
    TabArrayRetire *next = lj_tab_array_retired_next_acq(aret);
    array = lj_tab_array_retired_array_acq(aret);
    acap = lj_tab_array_retired_acap_acq(aret);
    if (lj_tab_array_retired_armed_acq(aret)) {
      if (LJ_UNLIKELY(!array ||
	  !lj_gc2_mem_registered_known(g, lj_tab_array_hdr(array)) ||
	  !tab_array_free_ready(array, acap))) {
	lj_assertG(0, "invalid terminal retired table-array body");
	abort();
      }
      tab_array_free(g, array, acap);
    }
    lj_mem_freet(g, aret);
    aret = next;
  }
}

static uint32_t countint(cTValue *key, uint32_t *bins)
{
  lj_assertX(!tvisint(key), "bad integer key");
  if (tvisnum(key)) {
    int64_t i64;
    int32_t k;
    if (lj_num2int_cond(numV(key), i64, k, (uint32_t)i64 < LJ_MAX_ASIZE)) {
      bins[(k > 2 ? lj_fls((uint32_t)(k-1)) : 0)]++;
      return 1;
    }
  }
  return 0;
}

static uint32_t countarray(const GCtab *t, uint32_t *bins)
{
  TValue *array;
  uint32_t na, b, i, asize = (uint32_t)lj_tab_array_snapshot_acq(t, &array);
  if (asize == 0) return 0;
  for (na = i = b = 0; b < LJ_MAX_ABITS; b++) {
    uint32_t n, top = 2u << b;
    if (top >= asize) {
      top = asize-1;
      if (i > top)
	break;
    }
    for (n = 0; i <= top; i++) {
      TValue val;
      lj_tv_load_acq(&val, &array[i]);
      if (!tab_val_absent(&val) && tab_tv_snapshot_valid(&val))
	n++;
    }
    bins[b] += n;
    na += n;
  }
  return na;
}

static uint32_t counthash(const GCtab *t, uint32_t *bins, uint32_t *narray)
{
  MSize hmask;
  Node *node = lj_tab_node_snapshot_acq(t, &hmask);
  uint32_t total, na, i;
  for (total = na = 0, i = 0; i <= hmask; i++) {
    Node *n = &node[i];
    TValue key, val;
    lj_tv_load_acq(&val, &n->val);
    if (!tab_val_absent(&val) && tab_tv_snapshot_valid(&val)) {
      lj_tv_load_acq(&key, &n->key);
      if (tab_tv_snapshot_valid(&key) && !tab_hash_key_hidden(&key)) {
	na += countint(&key, bins);
	total++;
      }
    }
  }
  *narray += na;
  return total;
}

static uint32_t bestasize(uint32_t bins[], uint32_t *narray)
{
  uint32_t b, sum, na = 0, sz = 0, nn = *narray;
  for (b = 0, sum = 0; 2*nn > (1u<<b) && sum != nn; b++)
    if (bins[b] > 0 && 2*(sum += bins[b]) > (1u<<b)) {
      sz = (2u<<b)+1;
      na = sum;
    }
  *narray = sz;
  return na;
}

static LJ_AINLINE int tab_key_on_stack(lua_State *L, cTValue *key)
{
  uintptr_t p, lo, hi;
  if (!L || !key)
    return 0;
  p = (uintptr_t)(const void *)key;
  lo = (uintptr_t)(const void *)tvref(L->stack);
  hi = (uintptr_t)(const void *)tvref(L->maxstack);
  return p >= lo && p < hi;
}

typedef struct TabRootAnchor {
  TGState *tg;
  uint32_t idx;
} TabRootAnchor;

static cTValue *tab_anchor_rehash_key(lua_State *L, cTValue *key,
				      TabRootAnchor *anchor)
{
  TGState *tg;
  TValue *slot;
  /*
  ** Rehash and stale-generation retry paths can allocate or step the GC before
  ** the key reaches a published table slot. VM helpers can enter with the exact
  ** Lua frame top still held in registers, so L->top is not safe temporary
  ** storage here. Use TG-owned anchor slots instead; both collectors mark them
  ** directly and the storage survives allocation longjmps.
  */
  if (LJ_UNLIKELY(!tab_tv_snapshot_valid(key)))
    lj_err_msg(L, LJ_ERR_NILIDX);
  anchor->tg = NULL;
  anchor->idx = 0;
  lj_gc_pubroot(L, key);
  tg = L2TG(L);
  if (LJ_UNLIKELY(!tg))
    tg = G(L)->main_tg;
  lj_assertL(tg != NULL, "table rehash key anchor without TG");
  slot = lj_tg_root_anchor_push(L, tg, key, &anchor->idx);
  if (LJ_UNLIKELY(!slot))
    return key;
  anchor->tg = tg;
  lj_gc_pubroot(L, slot);
  return slot;
}

static void tab_unanchor_rehash_key(TabRootAnchor *anchor)
{
  if (anchor->tg)
    lj_tg_root_anchor_pop(anchor->tg, anchor->idx);
}

static void rehashtab(lua_State *L, GCtab *t, cTValue *ek)
{
  uint32_t bins[LJ_MAX_ABITS];
  uint32_t total, asize, na, i;
  for (i = 0; i < LJ_MAX_ABITS; i++) bins[i] = 0;
  asize = countarray(t, bins);
  total = 1 + asize;
  total += counthash(t, bins, &asize);
  asize += countint(ek, bins);
  na = bestasize(bins, &asize);
  total -= na;
  lj_tab_resize(L, t, asize, hsize2hbits(total));
}

static TValue *tab_newkey_impl(lua_State *L, GCtab *t, cTValue *key,
			       int key_anchored);

static TValue *tab_set_current_key(lua_State *L, GCtab *t, cTValue *key)
{
  /*
  ** Retry paths already hold a root-anchored key. Dispatching back through
  ** lj_tab_set() would rebuild a fresh C-local key for string/numeric fast
  ** paths and can recurse indefinitely if each stale-generation retry lands on
  ** another post-publication generation hand-off. Go straight to the current
  ** generation insert/lookup helper; only integral keys need the array-aware
  ** setter because tab_newkey_impl() handles the hash part.
  */
  if (tvisint(key)) {
    return lj_tab_setint(L, t, intV(key));
  } else if (tvisnum(key)) {
    int64_t i64;
    int32_t k;
    if (lj_num2int_check(numV(key), i64, k))
      return lj_tab_setint(L, t, k);
    if (tvisnan(key))
      lj_err_msg(L, LJ_ERR_NANIDX);
  } else if (tvisnil(key)) {
    lj_err_msg(L, LJ_ERR_NILIDX);
  }
  return tab_newkey_impl(L, t, key, 1);
}

static TValue *tab_set_anchored_key(lua_State *L, GCtab *t, cTValue *key)
{
  TabRootAnchor anchor;
  TValue *slot;
  key = tab_anchor_rehash_key(L, key, &anchor);
  slot = tab_set_current_key(L, t, key);
  tab_unanchor_rehash_key(&anchor);
  return slot;
}

static TValue *tab_rehash_forwarded_key(lua_State *L, GCtab *t, cTValue *key)
{
  TabRootAnchor anchor;
  TValue *slot;
  key = tab_anchor_rehash_key(L, key, &anchor);
  rehashtab(L, t, key);
  slot = tab_set_current_key(L, t, key);
  tab_unanchor_rehash_key(&anchor);
  return slot;
}

static void tab_rehash_no_free(lua_State *L, GCtab *t, cTValue *ek,
			       MSize oldhmask)
{
  MSize hmask;
  Node *node;
  /*
  ** A full hash vector may only need compaction if deleted/forwarded values
  ** are present. Rebuild first and force growth only when the current rebuilt
  ** vector still has no insert capacity; this avoids unneeded generation churn
  ** while guaranteeing no-free insertion makes progress.
  */
  rehashtab(L, t, ek);
  node = lj_tab_node_snapshot_acq(t, &hmask);
  if (hmask <= oldhmask && (hmask == 0 || lj_tab_node_freecount_acq(node) == 0)) {
    TValue *array;
    uint32_t growhbits = oldhmask > 0 ?
      lj_fls((uint32_t)oldhmask) + 2u : 1u;
    lj_tab_resize(L, t, (uint32_t)lj_tab_array_snapshot_acq(t, &array),
		  growhbits);
  }
}

static TValue *tab_rehash_no_free_key(lua_State *L, GCtab *t, cTValue *key,
				      MSize oldhmask)
{
  TabRootAnchor anchor;
  TValue *slot;
  key = tab_anchor_rehash_key(L, key, &anchor);
  tab_rehash_no_free(L, t, key, oldhmask);
  slot = tab_set_current_key(L, t, key);
  tab_unanchor_rehash_key(&anchor);
  return slot;
}

TValue *lj_tab_setint_forward(lua_State *L, GCtab *t, int32_t key)
{
  TValue *array, val;
  MSize asize, hmask;
  uint32_t hbits, nasize;
  asize = lj_tab_array_snapshot_acq(t, &array);
  (void)lj_tab_node_snapshot_acq(t, &hmask);
  hbits = hmask > 0 ? lj_fls((uint32_t)hmask) + 1u : 0;
  if (asize >= LJ_MAX_ASIZE) {
    /* Preserve semantics over layout: move this key out of the maxed array. */
    lj_tab_resize(L, t, (uint32_t)key, hbits);
    return lj_tab_setinth(L, t, key);
  }
  nasize = (uint32_t)asize + 1u;  /* Force a replacement array generation. */
  if ((MSize)key >= asize && (MSize)key + 1u > (MSize)nasize)
    nasize = (uint32_t)key + 1u;
  lj_tab_resize(L, t, nasize, hbits);
  asize = lj_tab_array_snapshot_acq(t, &array);
  if ((MSize)key < asize) {
    lj_tv_load_acq(&val, &array[key]);
    if (tvisforward(&val))
      lj_tab_storenilraw(&array[key]);
    return &array[key];
  }
  return lj_tab_setinth(L, t, key);
}

static void tab_rehash_chain_overflow(lua_State *L, GCtab *t, cTValue *ek,
				      MSize oldhmask)
{
  uint32_t growhbits = oldhmask > 0 ? lj_fls((uint32_t)oldhmask) + 2u : 1u;
  MSize hmask;
  TValue *array;
  rehashtab(L, t, ek);
  (void)lj_tab_node_snapshot_acq(t, &hmask);
  if (hmask <= oldhmask)
    lj_tab_resize(L, t, (uint32_t)lj_tab_array_snapshot_acq(t, &array),
		  growhbits);
}

static TValue *tab_rehash_chain_overflow_key(lua_State *L, GCtab *t,
					     cTValue *key, MSize oldhmask)
{
  TabRootAnchor anchor;
  TValue *slot;
  key = tab_anchor_rehash_key(L, key, &anchor);
  tab_rehash_chain_overflow(L, t, key, oldhmask);
  slot = tab_set_current_key(L, t, key);
  tab_unanchor_rehash_key(&anchor);
  return slot;
}

void lj_tab_reasize(lua_State *L, GCtab *t, uint32_t nasize)
{
  MSize hmask;
  (void)lj_tab_node_snapshot_acq(t, &hmask);
  lj_tab_resize(L, t, nasize+1, hmask > 0 ? lj_fls(hmask)+1 : 0);
}

/* -- Table getters ------------------------------------------------------- */

static LJ_AINLINE int tab_node_in_snapshot(Node *node, MSize hmask, Node *n)
{
  uintptr_t base = (uintptr_t)node;
  uintptr_t p = (uintptr_t)n;
  uintptr_t bytes = ((uintptr_t)hmask + 1u) * sizeof(Node);
  return p >= base && p - base < bytes &&
	 ((p - base) % sizeof(Node)) == 0;
}

cTValue * LJ_FASTCALL lj_tab_getinth(GCtab *t, int32_t key)
{
  TValue k;
  Node *node;
  MSize hmask;
  Node *n;
  int key_retry = 1;
  int forwarded_retry = 0;
  int chain_retry = 1;
  k.n = (lua_Number)key;
retry_lookup:
  forwarded_retry = 0;
  node = lj_tab_node_snapshot_acq(t, &hmask);
genlookup:
  if (hmask == 0) {
    if (forwarded_retry && tab_forwarded_lookup_retry(t, node)) {
      goto retry_lookup;
    }
    return NULL;
  }
  n = hashnum_node(node, hmask, &k);
  do {
    TValue nk;
    lj_tv_load_acq(&nk, &n->key);
    if (tvisnum(&nk) && nk.n == k.n) {
      TValue val;
      lj_tv_load_acq(&val, &n->val);
      if (tvisforward(&val) && tab_node_forward_hop(t, &node, &hmask)) {
	cTValue *tv = tab_forwarded_int_arrayslot(t, key);
	forwarded_retry = 1;
	if (tv) {
	  TValue fval;
	  lj_tv_load_acq(&fval, tv);
	  if (!tab_val_absent(&fval))
	    return tv;
	  goto genlookup;
	}
	goto genlookup;
      }
      if (tab_val_forward_retry(t, &val, node))
	goto retry_lookup;
      if (forwarded_retry && tvisnil(&val) &&
	  tab_forwarded_lookup_retry(t, node))
	goto retry_lookup;
      if (tvisforward(&val))
	return NULL;
      return &n->val;
    }
    if (tab_key_read_retry_once(&nk, &key_retry))
      goto retry_lookup;
    n = lj_tab_nextnode_acq(n);
    if (LJ_UNLIKELY(n && !tab_node_in_snapshot(node, hmask, n))) {
      if (chain_retry-- > 0 || tab_forwarded_lookup_retry(t, node)) {
	lj_tab_wait_no_l();
	goto retry_lookup;
      }
      return NULL;
    }
  } while (n);
  if (forwarded_retry && tab_forwarded_lookup_retry(t, node)) {
    goto retry_lookup;
  }
  return NULL;
}

cTValue * LJ_FASTCALL lj_tab_getint_hop(GCtab *t, int32_t key)
{
  return lj_tab_getint(t, key);
}

int lj_tab_getstr_held_try(global_State *g, GCtab *t, const GCstr *key,
			   TValue *out)
{
  Node *node, *n;
  MSize hmask, visited = 0;
  int status;
  if (!g || !t || !key || !out)
    return LJ_TAB_GC_LOOKUP_RETRY;
  status = lj_tab_node_snapshot_gc_held(g, t, &node, &hmask);
  if (status != LJ_TAB_GC_SNAPSHOT_OK)
    return LJ_TAB_GC_LOOKUP_RETRY;
  if (hmask == 0)
    return LJ_TAB_GC_LOOKUP_ABSENT;
  n = hashstr_node(node, hmask, key);
  do {
    TValue nk;
    Node *next;
    if (LJ_UNLIKELY(!tab_node_in_snapshot(node, hmask, n) ||
		    visited++ > hmask))
      return LJ_TAB_GC_LOOKUP_RETRY;
    lj_tv_load_acq(&nk, &n->key);
    if (LJ_UNLIKELY(tab_key_islocked(&nk)))
      return LJ_TAB_GC_LOOKUP_RETRY;
    if (tvisstr(&nk) && strV(&nk) == key) {
      TValue val;
      lj_tv_load_acq(&val, &n->val);
      if (LJ_UNLIKELY(tvisforward(&val) ||
		      tab_val_is_publish_claim(&val)))
	return LJ_TAB_GC_LOOKUP_RETRY;
      if (LJ_UNLIKELY(lj_tab_node_acq(t) != node ||
		      lj_tab_node_is_retiring(node)))
	return LJ_TAB_GC_LOOKUP_RETRY;
      if (tvisnil(&val))
	return LJ_TAB_GC_LOOKUP_ABSENT;
      *out = val;
      return LJ_TAB_GC_LOOKUP_FOUND;
    }
    next = lj_tab_nextnode_acq(n);
    if (LJ_UNLIKELY(next && !tab_node_in_snapshot(node, hmask, next)))
      return LJ_TAB_GC_LOOKUP_RETRY;
    n = next;
  } while (n);
  if (LJ_UNLIKELY(lj_tab_node_acq(t) != node ||
		  lj_tab_node_is_retiring(node)))
    return LJ_TAB_GC_LOOKUP_RETRY;
  return LJ_TAB_GC_LOOKUP_ABSENT;
}

cTValue *lj_tab_getstr(GCtab *t, const GCstr *key)
{
  Node *node;
  MSize hmask;
  Node *n;
  int key_retry = 1;
  int forwarded_retry = 0;
  int chain_retry = 1;
retry_lookup:
  forwarded_retry = 0;
  node = lj_tab_node_snapshot_acq(t, &hmask);
genlookup:
  if (hmask == 0) {
    if (forwarded_retry && tab_forwarded_lookup_retry(t, node)) {
      goto retry_lookup;
    }
    return NULL;
  }
  n = hashstr_node(node, hmask, key);
  do {
    TValue nk;
    lj_tv_load_acq(&nk, &n->key);
    if (tvisstr(&nk) && strV(&nk) == key) {
      TValue val;
      lj_tv_load_acq(&val, &n->val);
      if (tvisforward(&val) && tab_node_forward_hop(t, &node, &hmask)) {
	forwarded_retry = 1;
	goto genlookup;
      }
      if (tab_val_forward_retry(t, &val, node))
	goto retry_lookup;
      if (forwarded_retry && tvisnil(&val) &&
	  tab_forwarded_lookup_retry(t, node))
	goto retry_lookup;
      if (tvisforward(&val))
	return NULL;
      return &n->val;
    }
    if (tab_key_read_retry_once(&nk, &key_retry))
      goto retry_lookup;
    n = lj_tab_nextnode_acq(n);
    if (LJ_UNLIKELY(n && !tab_node_in_snapshot(node, hmask, n))) {
      if (chain_retry-- > 0 || tab_forwarded_lookup_retry(t, node)) {
	lj_tab_wait_no_l();
	goto retry_lookup;
      }
      return NULL;
    }
  } while (n);
  if (forwarded_retry && tab_forwarded_lookup_retry(t, node)) {
    goto retry_lookup;
  }
  return NULL;
}

cTValue *lj_tab_get(lua_State *L, GCtab *t, cTValue *key)
{
  int key_retry = 1;
  if (LJ_UNLIKELY(!lj_gc_tv_gcref_valid(G(L), key)))
    return niltv(L);
  if (tvisstr(key)) {
    cTValue *tv = lj_tab_getstr(t, strV(key));
    if (tv)
      return tv;
  } else if (tvisint(key)) {
    cTValue *tv = lj_tab_getint(t, intV(key));
    if (tv)
      return tv;
  } else if (tvisnum(key)) {
    int64_t i64;
    int32_t k;
    if (lj_num2int_check(numV(key), i64, k)) {
      cTValue *tv = lj_tab_getint(t, k);
      if (tv)
	return tv;
    } else {
      goto retry_lookup;  /* Else use the generic lookup. */
    }
  } else if (!tvisnil(key)) {
    Node *node;
    MSize hmask;
    Node *n;
    int forwarded_retry = 0;
    int chain_retry = 1;
  retry_lookup:
    forwarded_retry = 0;
    node = lj_tab_node_snapshot_acq(t, &hmask);
  genlookup:
    if (hmask == 0) {
      if (forwarded_retry && tab_forwarded_lookup_retry(t, node)) {
	goto retry_lookup;
      }
      return niltv(L);
    }
    n = hashkey_node(node, hmask, key);
    do {
      TValue nk;
      lj_tv_load_acq(&nk, &n->key);
      if (lj_obj_equal(&nk, key)) {
	TValue val;
	lj_tv_load_acq(&val, &n->val);
	if (tvisforward(&val) && tab_node_forward_hop(t, &node, &hmask)) {
	  forwarded_retry = 1;
	  goto genlookup;
	}
	if (tab_val_forward_retry(t, &val, node))
	  goto retry_lookup;
	if (forwarded_retry && tvisnil(&val) &&
	    tab_forwarded_lookup_retry(t, node))
	  goto retry_lookup;
	if (tvisforward(&val))
	  return niltv(L);
	return &n->val;
      }
      if (tab_key_read_retry_once(&nk, &key_retry))
	goto retry_lookup;
      n = lj_tab_nextnode_acq(n);
      if (LJ_UNLIKELY(n && !tab_node_in_snapshot(node, hmask, n))) {
	if (chain_retry-- > 0 || tab_forwarded_lookup_retry(t, node)) {
	  lj_tab_wait_no_l();
	  goto retry_lookup;
	}
	return niltv(L);
      }
    } while (n);
    if (forwarded_retry && tab_forwarded_lookup_retry(t, node)) {
      goto retry_lookup;
    }
  }
  return niltv(L);
}

static LJ_AINLINE void tab_publish_storage(lua_State *L, GCtab *t);

typedef struct TabForjitSnapshot {
  TValue *array;
  MSize asize;
  Node *node;
  MSize hmask;
} TabForjitSnapshot;

enum {
  TAB_RESOLVE_RETRY = -1,
  TAB_RESOLVE_ABSENT = 0,
  TAB_RESOLVE_FOUND = 1
};

/* Capture both table side vectors as one retryable logical generation. A
** resize publishes the array and hash roots separately, so validating only
** the side which contained the key can accept a mixed old/new snapshot and
** manufacture a false miss while an integral key moves hash -> array. The SMR
** reader held by the caller prevents either captured vector from being freed;
** the paired root/metadata recheck below supplies the semantic linearization. */
static int tab_forjit_snapshot_acq(GCtab *t, TabForjitSnapshot *snap)
{
  TValue *array = lj_tab_array_acq(t);
  Node *node;
  MSize asize, hmask;
  if (lj_tab_array_is_retiring(t, array))
    return 0;
  asize = array && !lj_tab_array_is_colocated(t, array) ?
    lj_tab_array_hdr_asize_acq(array) : lj_tab_asize_acq(t);
  node = lj_tab_node_acq(t);
  hmask = lj_tab_node_hmask_acq(node);
  if (lj_tab_node_is_retiring(node) || !lj_tab_hmask_value_valid(hmask))
    return 0;
  snap->array = array;
  snap->asize = asize;
  snap->node = node;
  snap->hmask = hmask;
  return 1;
}

static int tab_forjit_snapshot_current(GCtab *t,
				       const TabForjitSnapshot *snap)
{
  if (lj_tab_array_acq(t) != snap->array ||
      lj_tab_node_acq(t) != snap->node ||
      lj_tab_array_is_retiring(t, snap->array) ||
      lj_tab_node_is_retiring(snap->node) ||
      lj_tab_node_hmask_acq(snap->node) != snap->hmask)
    return 0;
  if (snap->array && !lj_tab_array_is_colocated(t, snap->array))
    return lj_tab_array_hdr_asize_acq(snap->array) == snap->asize;
  return lj_tab_asize_acq(t) == snap->asize;
}

/* Resolve one key entirely inside a caller-owned SMR interval. The returned
** slot is an opaque address hint: callers must close this interval before any
** wait/allocation and must revalidate the hint in the keyed store transaction
** before dereferencing it. out, when non-NULL, is copied while the vectors are
** still retained by SMR. */
static int tab_resolve_current_keyed_held(GCtab *t, cTValue *key,
					 TValue **slotp, TValue *out)
{
  TabForjitSnapshot snap;
  TValue hkey;
  cTValue *hkeyp = key;
  Node *n;
  MSize visited = 0;
  int64_t i64;
  int32_t ik = 0;
  int isnumkey = 0;
  int isintkey = 0;
  TValue *slot = NULL;
  int result = TAB_RESOLVE_ABSENT;

  /* Nil is a terminal absent key, not a generation retry. */
  if (tvisnil(key)) {
    if (slotp) *slotp = NULL;
    if (out) setnilV(out);
    return TAB_RESOLVE_ABSENT;
  }
  if (!tab_forjit_snapshot_acq(t, &snap))
    return TAB_RESOLVE_RETRY;

  if (tvisint(key)) {
    ik = intV(key);
    setnumV(&hkey, (lua_Number)ik);
    hkeyp = &hkey;
    isnumkey = 1;
    isintkey = 1;
  } else if (tvisnum(key)) {
    isnumkey = 1;
    isintkey = lj_num2int_check(numV(key), i64, ik);
  }

  /* Current integral keys live in the array side when in range. Check it
  ** before the hash side, then validate both roots together below. */
  if (isintkey && (MSize)ik < snap.asize) {
    TValue val;
    if (!snap.array)
      return TAB_RESOLVE_RETRY;
    slot = &snap.array[ik];
    lj_tv_load_acq(&val, slot);
    if (tvisforward(&val) || tab_val_is_publish_claim(&val))
      return TAB_RESOLVE_RETRY;
    if (out) *out = val;
    result = TAB_RESOLVE_FOUND;
    goto validate;
  }
  if (isintkey)
    tab_forjit_test_pause_after_integral_array();

  if (snap.hmask == 0)
    goto absent;
  n = isnumkey ? hashnum_node(snap.node, snap.hmask, hkeyp) :
      tvisstr(hkeyp) ? hashstr_node(snap.node, snap.hmask, strV(hkeyp)) :
      hashkey_node(snap.node, snap.hmask, hkeyp);
  do {
    TValue nk;
    Node *next;
    if (LJ_UNLIKELY(!tab_node_in_snapshot(snap.node, snap.hmask, n) ||
		    visited++ > snap.hmask))
      return TAB_RESOLVE_RETRY;
    lj_tv_load_acq(&nk, &n->key);
    if (tab_key_islocked(&nk))
      return TAB_RESOLVE_RETRY;
    if ((isnumkey && tvisnum(&nk) && nk.n == hkeyp->n) ||
	(tvisstr(hkeyp) && tvisstr(&nk) && strV(&nk) == strV(hkeyp)) ||
	(!isnumkey && !tvisstr(hkeyp) && lj_obj_equal(&nk, hkeyp))) {
      TValue val;
      slot = &n->val;
      lj_tv_load_acq(&val, slot);
      if (tvisforward(&val) || tab_val_is_publish_claim(&val))
	return TAB_RESOLVE_RETRY;
      if (out) *out = val;
      result = TAB_RESOLVE_FOUND;
      goto validate;
    }
    next = lj_tab_nextnode_acq(n);
    if (LJ_UNLIKELY(next &&
		    !tab_node_in_snapshot(snap.node, snap.hmask, next)))
      return TAB_RESOLVE_RETRY;
    n = next;
  } while (n);

absent:
  if (out) setnilV(out);
  result = TAB_RESOLVE_ABSENT;
validate:
  if (!tab_forjit_snapshot_current(t, &snap))
    return TAB_RESOLVE_RETRY;
  if (slotp) *slotp = slot;
  return result;
}

static int tab_get_current_forjit(GCtab *t, cTValue *key, TValue *out)
{
  int status = tab_resolve_current_keyed_held(t, key, NULL, out);
  if (status == TAB_RESOLVE_FOUND)
    return tab_val_absent(out) ? 0 : 1;
  return status;
}

static int tab_resolve_current_keyed_try(global_State *g, GCtab *t,
					  cTValue *key, TValue **slotp)
{
  int status;
  *slotp = NULL;
  if (!g || !t || !key || !lj_gc2_smr_read_try(g))
    return TAB_RESOLVE_RETRY;
  status = tab_resolve_current_keyed_held(t, key, slotp, NULL);
  lj_gc2_smr_read_leave(g);
  return status;
}

LJ_FUNCA TValue *lj_tab_gettv_forjit(lua_State *L, GCtab *t, cTValue *key,
				     TValue *out)
{
  for (;;) {
    LJGC2Lease table_lease, key_lease, result_lease;
    TValue tablev;
    int table_status, key_status, result_status;

    /* Hashing a GC key dereferences its exact string/cdata/object identity.
    ** Likewise every vector snapshot starts from the table body. Retain both
    ** allocations across the initial lookup and any current-generation miss
    ** resolution; validation without this lease has a use-after-validation
    ** gap when GC2 owns a lifetime transition. */
    /* Synthetic tag only: assertion-enabled settabV would read t->gct before
    ** the lifetime lease which is meant to validate this possibly raced input. */
    setgcVraw(&tablev, obj2gco(t), LJ_TTAB);
    table_status = lj_gc2_tv_lease_acquire(G(L), &tablev, &table_lease);
    if (LJ_UNLIKELY(table_status != LJ_GC2_TV_EDGE_VALID)) {
      if (table_status == LJ_GC2_TV_EDGE_RETRY) {
	lj_tab_wait_l(L);
	continue;
      }
      setnilV(out);
      return out;
    }
    key_status = lj_gc2_tv_lease_acquire(G(L), key, &key_lease);
    if (LJ_UNLIKELY(key_status != LJ_GC2_TV_EDGE_VALID)) {
      lj_gc2_lease_release(&table_lease);
      if (key_status == LJ_GC2_TV_EDGE_RETRY) {
	lj_tab_wait_l(L);
	continue;
      }
      setnilV(out);
      return out;
    }

    tab_forjit_test_pause_after_leases();
    if (LJ_UNLIKELY(!lj_gc2_smr_read_try(G(L)))) {
      lj_gc2_lease_release(&key_lease);
      lj_gc2_lease_release(&table_lease);
      lj_tab_wait_l(L);
      continue;
    }
    /* The old two-stage lookup could call a yielding resolver while this SMR
    ** reader was active. Resolve exactly once from a paired current-root
    ** snapshot instead; RETRY is handled only after the reader is closed. */
    if (tab_forjit_test_take_initial_miss() ||
	tab_get_current_forjit(t, key, out) < 0) {
      lj_gc2_smr_read_leave(G(L));
      lj_gc2_lease_release(&key_lease);
      lj_gc2_lease_release(&table_lease);
      lj_tab_wait_l(L);
      continue;
    }
    /* The copied slot stops being protected by the vector SMR scope below.
    ** Admit its exact GC allocation before closing that scope, so a weak clear
    ** or concurrent sweep cannot reclaim/reuse the body between copy, header
    ** validation and publication. RETRY is a transient ownership collision;
    ** STALE is a terminal old incarnation and therefore reads as nil. */
    result_status = lj_gc2_tv_lease_acquire(G(L), out, &result_lease);
    if (LJ_UNLIKELY(result_status != LJ_GC2_TV_EDGE_VALID)) {
      if (result_status == LJ_GC2_TV_EDGE_STALE)
	setnilV(out);
      lj_gc2_smr_read_leave(G(L));
      lj_gc2_lease_release(&key_lease);
      lj_gc2_lease_release(&table_lease);
      if (result_status == LJ_GC2_TV_EDGE_STALE)
	return out;
      lj_tab_wait_l(L);
      continue;
    }
    tab_forjit_test_pause_after_result_lease(out);
    lj_gc2_smr_read_leave(G(L));
    if (LJ_LIKELY(tab_tv_forjit_loadable(out))) {
      /*
      ** Helper-backed trace reads return through a temporary TValue, not a Lua
      ** stack slot. Publish GC values and the source table before later trace
      ** helpers can allocate or assist GC with only the temporary live.
      */
      if (tvisgcv(out)) {
	lj_gc_pubroot(L, out);
	tab_publish_storage(L, t);
	lj_gc_pubtabtv(L, t, out);
	lj_gc_pubtab(L, t);
      }
      lj_gc2_lease_release(&result_lease);
      lj_gc2_lease_release(&key_lease);
      lj_gc2_lease_release(&table_lease);
      return out;
    }
    lj_gc2_lease_release(&result_lease);
    lj_gc2_lease_release(&key_lease);
    lj_gc2_lease_release(&table_lease);
    lj_tab_wait_l(L);
  }
}

/* -- Table setters ------------------------------------------------------- */

static int tab_try_claim_nil_key(TValue *dst)
{
  TValue expect, keylock;
  setnilV(&expect);
  setkeylockV(&keylock);
  if (lj_tv_cas(dst, &expect, &keylock))
    return 1;
  return tviskeylock(&expect) ? -1 : 0;
}

static LJ_AINLINE int tab_hash_generation_current(GCtab *t, const Node *nodebase)
{
  return lj_tab_node_acq(t) == nodebase && !lj_tab_node_is_retiring(nodebase);
}

static LJ_AINLINE int tab_node_free_take_private(Node *node)
{
  TabNodeHdr *hdr = lj_tab_node_hdrw(node);
  uint32_t old = hdr->flags;
  uint32_t count = old & (uint32_t)TABNODE_FREECOUNT_MASK;
  if ((old & (uint32_t)TABNODE_FLAG_RETIRING) || count == 0)
    return 0;
  hdr->flags = (old & (uint32_t)TABNODE_FLAGS_MASK) | (count - 1u);
  return 1;
}

static LJ_AINLINE int tab_node_free_private(Node *n)
{
  TValue nk, nv;
  lj_tv_load_acq(&nk, &n->key);
  if (!tvisnil(&nk))
    return 0;
  lj_tv_load_acq(&nv, &n->val);
  return tvisnil(&nv);
}

static Node *tab_find_free_node_scan_private(Node *nodebase, MSize hmask,
					     const Node *anchor)
{
  MSize start = (MSize)(anchor - nodebase);
  MSize i;
  for (i = 1; i <= hmask; i++) {
    MSize idx = (start + i) & hmask;
    Node *n = &nodebase[idx];
    if (tab_node_free_private(n))
      return n;
  }
  return NULL;
}

static Node *tab_find_free_node_private(GCtab *t, Node *nodebase, MSize hmask,
					const Node *anchor)
{
  Node *limit = &nodebase[hmask+1];
  Node *top = getfreetop(t, nodebase);
  /*
  ** In the private single-mutator window, freetop is a cheap cursor over the
  ** currently published hash vector. It is only a hint: shared insertions and
  ** abandoned claims do not maintain it, so keep the old full scan as the
  ** correctness fallback. A reusable hash node must have both key and value
  ** nil; tombstone anchors and unpublished value claims are not free nodes.
  */
  if (top > nodebase && top <= limit) {
    while (top > nodebase) {
      Node *n = top - 1;
      top = n;
      if (n == anchor)
	continue;
      if (tab_node_free_private(n)) {
	setfreetop(t, nodebase, n);
	return n;
      }
    }
  }
  return tab_find_free_node_scan_private(nodebase, hmask, anchor);
}

static TValue *tab_newkey_private_empty_anchor(lua_State *L, GCtab *t,
					       cTValue *key,
					       Node *nodebase, Node *anchor)
{
  TValue nk, nv;
  /*
  ** A private empty anchor with no collision chain can be claimed before the
  ** generic duplicate scan: no chain means there is no duplicate key to skip.
  ** Reload everything after the private/current-generation predicate so active
  ** MT, GC workers, active marking, KEYLOCK claims, resize, tombstones, and
  ** collision chains still fall through to the normal protocol below.
  */
  if (!tab_private_mutation_allowed(L) ||
      !tab_hash_generation_current(t, nodebase))
    return NULL;
  lj_tv_load_acq(&nk, &anchor->key);
  if (!tvisnil(&nk))
    return NULL;
  lj_tv_load_acq(&nv, &anchor->val);
  if (!tvisnil(&nv) || lj_tab_nextnode_acq(anchor) != NULL)
    return NULL;
  if (!tab_node_free_take_private(nodebase))
    return NULL;
  tab_storekeyrel(L, &anchor->key, key);
  lj_gc2_barrier_weak_key(L, t, key);
  lj_gc_pubtabkey(L, t, key);
  lj_assertL(lj_tv_isnil_acq(&anchor->val), "new hash slot is not empty");
  return &anchor->val;
}

static TValue *tab_newkey_private(lua_State *L, GCtab *t, cTValue *key,
				  Node *nodebase, MSize hmask, Node *anchor,
				  int *chain_overflow)
{
  TValue nk, nv;
  Node *n;
  MSize chainlen = 0;
  *chain_overflow = 0;
  if (!tab_private_mutation_allowed(L) ||
      !tab_hash_generation_current(t, nodebase))
    return NULL;
  /*
  ** Private single-mutator insertion has no racing key publisher, so scan the
  ** stable collision chain here instead of entering the shared KEYLOCK/CAS
  ** lookup first. Still reload after the private predicate and keep the normal
  ** chain limit, generation check, key canonicalization, and GC key barriers.
  */
  for (n = anchor; n != NULL; n = lj_tab_nextnode_acq(n)) {
    chainlen++;
    lj_tv_load_acq(&nk, &n->key);
    if (lj_obj_equal(&nk, key))
      return &n->val;
    if (tab_key_islocked(&nk))
      return NULL;
  }
  if (chainlen >= LJ_TAB_MAXCHAIN) {
    *chain_overflow = 1;
    return NULL;
  }
  if (!tab_hash_generation_current(t, nodebase))
    return NULL;
  lj_tv_load_acq(&nk, &anchor->key);
  lj_tv_load_acq(&nv, &anchor->val);
  if (tvisnil(&nk) && tvisnil(&nv)) {
    if (!tab_node_free_take_private(nodebase))
      return NULL;
    tab_storekeyrel(L, &anchor->key, key);
    lj_gc2_barrier_weak_key(L, t, key);
    lj_gc_pubtabkey(L, t, key);
    lj_assertL(lj_tv_isnil_acq(&anchor->val), "new hash slot is not empty");
    return &anchor->val;
  } else {
    Node *freenode;
    if (!tab_node_free_take_private(nodebase))
      return NULL;
    freenode = tvisnil(&nv) ?
	       tab_find_free_node_scan_private(nodebase, hmask, anchor) :
	       tab_find_free_node_private(t, nodebase, hmask, anchor);
    if (!freenode) {
      lj_tab_node_free_release(nodebase);
      return NULL;
    }
    lj_tab_nextnode_set(freenode, lj_tab_nextnode_acq(anchor));
    lj_tab_nextnode_set(anchor, freenode);
    tab_storekeyrel(L, &freenode->key, key);
    lj_gc2_barrier_weak_key(L, t, key);
    lj_gc_pubtabkey(L, t, key);
    lj_assertL(lj_tv_isnil_acq(&freenode->val),
	       "new hash slot is not empty");
    return &freenode->val;
  }
}

#ifdef LJ_TAB_TEST_HELPERS
static LJTabNewkeyReserveHook tab_test_newkey_anchor_after_reserve_hook;
static LJTabNewkeyReserveHook tab_test_newkey_chain_after_reserve_hook;
static LJTabNextAfterKeyindexHook tab_test_next_after_keyindex_hook;
static LJTabStorePostCasHook tab_test_store_post_cas_hook;

void lj_tab_test_set_newkey_anchor_after_reserve_hook(
  LJTabNewkeyReserveHook hook)
{
  tab_test_newkey_anchor_after_reserve_hook = hook;
}

void lj_tab_test_set_newkey_chain_after_reserve_hook(
  LJTabNewkeyReserveHook hook)
{
  tab_test_newkey_chain_after_reserve_hook = hook;
}

void lj_tab_test_set_resize_colocated_after_freeze_hook(
  LJTabResizeArrayHook hook)
{
  tab_test_resize_colocated_after_freeze_hook = hook;
}

void lj_tab_test_set_next_after_keyindex_hook(
  LJTabNextAfterKeyindexHook hook)
{
  tab_test_next_after_keyindex_hook = hook;
}

void lj_tab_test_set_store_post_cas_hook(LJTabStorePostCasHook hook)
{
  tab_test_store_post_cas_hook = hook;
}

static LJ_AINLINE void tab_test_newkey_anchor_after_reserve(lua_State *L,
							    GCtab *t,
							    Node *nodebase)
{
  if (tab_test_newkey_anchor_after_reserve_hook)
    tab_test_newkey_anchor_after_reserve_hook(L, t, nodebase);
}

static LJ_AINLINE void tab_test_newkey_chain_after_reserve(lua_State *L,
							   GCtab *t,
							   Node *nodebase)
{
  if (tab_test_newkey_chain_after_reserve_hook)
    tab_test_newkey_chain_after_reserve_hook(L, t, nodebase);
}

static LJ_AINLINE void tab_test_next_after_keyindex(GCtab *t, uint32_t idx)
{
  if (tab_test_next_after_keyindex_hook)
    tab_test_next_after_keyindex_hook(t, idx);
}

static LJ_AINLINE void tab_test_store_post_cas(lua_State *L, GCtab *t,
						TValue *dst, cTValue *key,
						cTValue *value)
{
  if (tab_test_store_post_cas_hook)
    tab_test_store_post_cas_hook(L, t, dst, key, value);
}

#else
#define tab_test_newkey_anchor_after_reserve(L, t, nodebase) \
  ((void)(L), (void)(t), (void)(nodebase))
#define tab_test_newkey_chain_after_reserve(L, t, nodebase) \
  ((void)(L), (void)(t), (void)(nodebase))
#define tab_test_next_after_keyindex(t, idx) \
  ((void)(t), (void)(idx))
#define tab_test_store_post_cas(L, t, dst, key, value) \
  ((void)(L), (void)(t), (void)(dst), (void)(key), (void)(value))
#endif

static void tab_release_claimed_anchor(Node *nodebase, Node *n)
{
  lj_tab_storenilraw(&n->key);
  lj_tab_node_free_release(nodebase);
}

static void tab_release_claimed_anchor_value(Node *nodebase, Node *n)
{
  lj_tab_storenilraw(&n->val);
  lj_tab_storenilraw(&n->key);
  lj_tab_node_free_release(nodebase);
}

static void tab_release_claimed_free(Node *nodebase, Node *n)
{
  lj_tab_nextnode_set(n, NULL);
  lj_tab_storenilraw(&n->val);
  lj_tab_storenilraw(&n->key);
  lj_tab_node_free_release(nodebase);
}

static Node *tab_claim_free_node_scan(Node *nodebase, MSize hmask,
				      const Node *anchor, int *locked)
{
  MSize start = (MSize)(anchor - nodebase);
  MSize i;
  int reserved = lj_tab_node_free_reserve(nodebase);
  *locked = 0;
  if (reserved <= 0) {
    if (reserved < 0)
      *locked = 1;
    return NULL;
  }
  for (i = 1; i <= hmask; i++) {
    MSize idx = (start + i) & hmask;
    Node *n = &nodebase[idx];
    TValue nk;
    lj_tv_load_acq(&nk, &n->key);
    if (tab_key_islocked(&nk)) {
      lj_tab_node_free_release(nodebase);
      *locked = 1;
      return NULL;
    }
    if (tvisnil(&nk) && lj_tv_isnil_acq(&n->val)) {
      int claimed = tab_try_claim_nil_key(&n->key);
      if (claimed < 0) {
	lj_tab_node_free_release(nodebase);
	*locked = 1;
	return NULL;
      }
      if (claimed == 1)
	return n;
    }
  }
  lj_tab_node_free_release(nodebase);
  return NULL;
}

/* Insert new key. Nodes are never moved within a hash generation. */
static TValue *tab_newkey_impl(lua_State *L, GCtab *t, cTValue *key,
			       int key_anchored)
{
  global_State *g = G(L);
  Node *nodebase;
  MSize hmask;
  Node *n;
retry_insert:
  /* This interval owns every raw node/header dereference in the insertion
  ** attempt. All waits, resize allocation and recursive retry paths below
  ** leave it first; publication inside the interval is bounded and does not
  ** call an L-aware safepoint. */
  if (LJ_UNLIKELY(!lj_gc2_smr_read_try(g))) {
    lj_tab_wait_l(L);
    goto retry_insert;
  }
  nodebase = lj_tab_node_snapshot_acq(t, &hmask);
  if (hmask == 0) {
    lj_gc2_smr_read_leave(g);
    if (key_anchored) {
      rehashtab(L, t, key);
      goto retry_insert;
    }
    return tab_rehash_forwarded_key(L, t, key);
  }
  n = hashkey_node(nodebase, hmask, key);
  {
    TValue *slot = tab_newkey_private_empty_anchor(L, t, key, nodebase, n);
    if (slot) {
      lj_gc2_smr_read_leave(g);
      return slot;
    }
  }
  if (tab_private_mutation_allowed(L)) {
    int chain_overflow;
    TValue *slot = tab_newkey_private(L, t, key, nodebase, hmask, n,
				      &chain_overflow);
    if (slot) {
      lj_gc2_smr_read_leave(g);
      return slot;
    }
    if (chain_overflow) {
      lj_gc2_smr_read_leave(g);
      if (key_anchored) {
	tab_rehash_chain_overflow(L, t, key, hmask);
	goto retry_insert;
      }
      return tab_rehash_chain_overflow_key(L, t, key, hmask);
    }
  }
  {
    int locked;
    MSize chainlen;
    TValue *slot = tab_findkey_or_keylock(n, key, &locked, &chainlen);
    if (slot) {
      lj_gc2_smr_read_leave(g);
      return slot;
    }
    if (locked) {
      lj_gc2_smr_read_leave(g);
      lj_tab_wait_no_l();
      goto retry_insert;
    }
    if (chainlen >= LJ_TAB_MAXCHAIN) {
      lj_gc2_smr_read_leave(g);
      if (key_anchored) {
	tab_rehash_chain_overflow(L, t, key, hmask);
	goto retry_insert;
      }
      return tab_rehash_chain_overflow_key(L, t, key, hmask);
    }
  }
  {
    TValue nk, nv;
    lj_tv_load_acq(&nk, &n->key);
    if (tab_key_islocked(&nk)) {
      lj_gc2_smr_read_leave(g);
      lj_tab_wait_no_l();
      goto retry_insert;
    }
    lj_tv_load_acq(&nv, &n->val);
    if (tvisnil(&nk) && tvisnil(&nv)) {
      int reserved = lj_tab_node_free_reserve(nodebase);
      if (reserved < 0) {
	lj_gc2_smr_read_leave(g);
	lj_tab_wait_no_l();
	goto retry_insert;
      }
      if (reserved == 0) {
	lj_gc2_smr_read_leave(g);
	if (key_anchored) {
	  tab_rehash_no_free(L, t, key, hmask);
	  goto retry_insert;
	}
	return tab_rehash_no_free_key(L, t, key, hmask);
      }
      {
	int claimed = tab_try_claim_nil_key(&n->key);
	if (claimed < 0) {
	  lj_tab_node_free_release(nodebase);
	  lj_gc2_smr_read_leave(g);
	  lj_tab_wait_no_l();
	  goto retry_insert;
	}
	if (claimed == 1) {
	  if (!tab_hash_generation_current(t, nodebase)) {
	    tab_release_claimed_anchor(nodebase, n);
	    lj_gc2_smr_read_leave(g);
	    lj_tab_wait_no_l();
	    goto retry_insert;
	  }
	  tab_storekeyrel(L, &n->key, key);
	  lj_gc2_barrier_weak_key(L, t, key);
	  lj_gc_pubtabkey(L, t, key);
	  lj_assertL(lj_tv_isnil_acq(&n->val), "new hash slot is not empty");
	  if (!tab_hash_generation_current(t, nodebase)) {
	    lj_gc2_smr_read_leave(g);
	    lj_tab_wait_no_l();
	    if (key_anchored)
	      goto retry_insert;
	    return tab_set_anchored_key(L, t, key);
	  }
	  lj_gc2_smr_read_leave(g);
	  return &n->val;
	}
	lj_tab_node_free_release(nodebase);
      }

      lj_gc2_smr_read_leave(g);
      goto retry_insert;
    }
  }
  {
    int locked;
    Node *freenode = tab_claim_free_node_scan(nodebase, hmask, n, &locked);
    lj_assertL(nodebase != &G(L)->nilnode, "insert into fallback hash");
    if (!freenode) {
      if (locked) {
	lj_gc2_smr_read_leave(g);
	lj_tab_wait_no_l();
	goto retry_insert;
      }
      lj_gc2_smr_read_leave(g);
      if (key_anchored) {
	tab_rehash_no_free(L, t, key, hmask);
	goto retry_insert;
      }
      return tab_rehash_no_free_key(L, t, key, hmask);
    }
    {
      MSize chainlen;
      TValue *slot = tab_findkey_or_keylock(n, key, &locked, &chainlen);
      if (slot) {
	tab_release_claimed_free(nodebase, freenode);
	lj_gc2_smr_read_leave(g);
	return slot;
      }
      if (locked) {
	tab_release_claimed_free(nodebase, freenode);
	lj_gc2_smr_read_leave(g);
	lj_tab_wait_no_l();
	goto retry_insert;
      }
      if (chainlen >= LJ_TAB_MAXCHAIN) {
	tab_release_claimed_free(nodebase, freenode);
	lj_gc2_smr_read_leave(g);
	if (key_anchored) {
	  tab_rehash_chain_overflow(L, t, key, hmask);
	  goto retry_insert;
	}
	return tab_rehash_chain_overflow_key(L, t, key, hmask);
      }
    }
    lj_assertL(freenode != &G(L)->nilnode, "store to fallback hash");
    {
      Node *next;
      if (!tab_hash_generation_current(t, nodebase)) {
	tab_release_claimed_free(nodebase, freenode);
	lj_gc2_smr_read_leave(g);
	lj_tab_wait_no_l();
	goto retry_insert;
      }
      next = lj_tab_nextnode_acq(n);
      lj_tab_nextnode_set(freenode, next);
      if (!tab_nextnode_cas(n, &next, freenode)) {
	tab_release_claimed_free(nodebase, freenode);
	lj_gc2_smr_read_leave(g);
	lj_tab_wait_no_l();
	goto retry_insert;
      }
    }
    tab_storekeyrel(L, &freenode->key, key);
    lj_gc2_barrier_weak_key(L, t, key);
    lj_gc_pubtabkey(L, t, key);
    lj_assertL(lj_tv_isnil_acq(&freenode->val),
	       "new hash slot is not empty");
    if (!tab_hash_generation_current(t, nodebase)) {
      lj_gc2_smr_read_leave(g);
      lj_tab_wait_no_l();
      if (key_anchored)
	goto retry_insert;
      return tab_set_anchored_key(L, t, key);
    }
    lj_gc2_smr_read_leave(g);
    return &freenode->val;
  }
}

TValue *lj_tab_newkey(lua_State *L, GCtab *t, cTValue *key)
{
  TabRootAnchor anchor;
  TValue *slot;
  /*
  ** Missing-key insertion can wait on a key lock, publish weak-key barriers, or
  ** lose a generation race before the key reaches the table. Keep the key in
  ** TG-owned root storage for the whole slow path; L->top can lag the running
  ** interpreter frame and is not reliable scratch storage here. Keep the old
  ** retry control flow: integer keys in a zero-hash table may resize into the
  ** array part and must dispatch through the forwarded-key path.
  */
  key = tab_anchor_rehash_key(L, key, &anchor);
  slot = tab_newkey_impl(L, t, key, 0);
  tab_unanchor_rehash_key(&anchor);
  return slot;
}

int lj_tab_try_newkey_anchor(lua_State *L, GCtab *t, cTValue *key,
			     cTValue *claim, TValue **slot)
{
  global_State *g = G(L);
retry_attempt:
  if (LJ_UNLIKELY(!lj_gc2_smr_read_try(g))) {
    lj_tab_wait_l(L);
    goto retry_attempt;
  }
  for (;;) {
    MSize hmask;
    Node *nodebase = lj_tab_node_snapshot_acq(t, &hmask);
    Node *n;
    TValue nk, nv, expect;
    if (hmask == 0) {
      lj_gc2_smr_read_leave(g);
      return 0;
    }
    n = hashkey_node(nodebase, hmask, key);
    lj_tv_load_acq(&nk, &n->key);
    if (lj_obj_equal(&nk, key)) {
      lj_gc2_smr_read_leave(g);
      return -1;  /* A racing inserter published the key; retry lookup. */
    }
    if (tviskeylock(&nk)) {
      lj_gc2_smr_read_leave(g);
      lj_tab_wait_no_l();  /* Claimed anchor is publishing key. */
      goto retry_attempt;
    }
    if (!tvisnil(&nk)) {
      lj_gc2_smr_read_leave(g);
      return 0;  /* Caller handles collision-chain or resize fallback. */
    }
    lj_tv_load_acq(&nv, &n->val);
    if (!tvisnil(&nv)) {
      lj_gc2_smr_read_leave(g);
      lj_tab_wait_no_l();  /* Claimed anchor value is publishing. */
      goto retry_attempt;
    }
    {
      int reserved = lj_tab_node_free_reserve(nodebase);
      if (reserved <= 0) {
	lj_gc2_smr_read_leave(g);
	return 0;
      }
    }
    tab_test_newkey_anchor_after_reserve(L, t, nodebase);
    if (!tab_hash_generation_current(t, nodebase)) {
      lj_tab_node_free_release(nodebase);
      lj_gc2_smr_read_leave(g);
      return -1;
    }
    {
      int claimed = tab_try_claim_nil_key(&n->key);
      if (claimed < 0) {
	lj_tab_node_free_release(nodebase);
	lj_gc2_smr_read_leave(g);
	lj_tab_wait_no_l();
	goto retry_attempt;
      }
      if (claimed == 0) {
	lj_tab_node_free_release(nodebase);
	lj_gc2_smr_read_leave(g);
	lj_tab_wait_no_l();
	goto retry_attempt;
      }
    }
    if (!tab_hash_generation_current(t, nodebase)) {
      tab_release_claimed_anchor(nodebase, n);
      lj_gc2_smr_read_leave(g);
      return -1;
    }
    setnilV(&expect);
    if (lj_tv_cas(&n->val, &expect, claim)) {
      if (!tab_hash_generation_current(t, nodebase)) {
	tab_release_claimed_anchor_value(nodebase, n);
	lj_gc2_smr_read_leave(g);
	return -1;
      }
      tab_storekeyrel(L, &n->key, key);
      *slot = &n->val;
      lj_gc2_smr_read_leave(g);
      return 1;
    }
    tab_release_claimed_anchor(nodebase, n);
    lj_gc2_smr_read_leave(g);
    lj_tab_wait_no_l();
    goto retry_attempt;
  }
}

int lj_tab_try_newkey_chain(lua_State *L, GCtab *t, cTValue *key,
			    cTValue *claim, TValue **slot)
{
  global_State *g = G(L);
retry_attempt:
  if (LJ_UNLIKELY(!lj_gc2_smr_read_try(g))) {
    lj_tab_wait_l(L);
    goto retry_attempt;
  }
  {
    MSize hmask;
    Node *nodebase = lj_tab_node_snapshot_acq(t, &hmask);
    Node *anchor, *reserved = NULL;
    if (hmask == 0) {
      lj_gc2_smr_read_leave(g);
      return 0;
    }
    anchor = hashkey_node(nodebase, hmask, key);
  for (;;) {
    Node *n;
    MSize i;
    for (n = anchor; n != NULL; n = lj_tab_nextnode_acq(n)) {
      TValue nk, nv;
      lj_tv_load_acq(&nv, &n->val);
      lj_tv_load_acq(&nk, &n->key);
      if (lj_obj_equal(&nk, key)) {
	if (reserved)
	  tab_release_claimed_free(nodebase, reserved);
	lj_gc2_smr_read_leave(g);
	return -1;  /* Existing or racing insert for this key; retry lookup. */
      }
      if (tviskeylock(&nk) || (tvisnil(&nk) && tab_val_isclaim(&nv, claim))) {
	if (reserved)
	  tab_release_claimed_free(nodebase, reserved);
	lj_gc2_smr_read_leave(g);
	lj_tab_wait_no_l();  /* Linked insert is publishing key. */
	goto retry_attempt;
      }
    }
    if (!reserved) {
      int reserve = lj_tab_node_free_reserve(nodebase);
      if (reserve <= 0) {
	lj_gc2_smr_read_leave(g);
	return 0;
      }
      tab_test_newkey_chain_after_reserve(L, t, nodebase);
      if (!tab_hash_generation_current(t, nodebase)) {
	lj_tab_node_free_release(nodebase);
	lj_gc2_smr_read_leave(g);
	return -1;
      }
      for (i = 0; i <= hmask; i++) {
	TValue nk, nv, expect;
	n = &nodebase[i];
	if (n == anchor)
	  continue;
	lj_tv_load_acq(&nk, &n->key);
	if (lj_obj_equal(&nk, key))
	  goto found_existing;
	if (tviskeylock(&nk)) {
	  lj_tab_node_free_release(nodebase);
	  lj_gc2_smr_read_leave(g);
	  lj_tab_wait_no_l();  /* Free-node key is publishing. */
	  goto retry_attempt;
	}
	if (!tvisnil(&nk))
	  continue;
	lj_tv_load_acq(&nv, &n->val);
	if (!tvisnil(&nv)) {
	  if (tab_val_isclaim(&nv, claim)) {
	    lj_tab_node_free_release(nodebase);
	    lj_gc2_smr_read_leave(g);
	    lj_tab_wait_no_l();  /* Free-node value is publishing. */
	    goto retry_attempt;
	  }
	  continue;
	}
	{
	  int claimed = tab_try_claim_nil_key(&n->key);
	  if (claimed < 0) {
	    lj_tab_node_free_release(nodebase);
	    lj_gc2_smr_read_leave(g);
	    lj_tab_wait_no_l();  /* Free-node key is publishing. */
	    goto retry_attempt;
	  }
	  if (claimed == 0)
	    continue;
	}
	if (!tab_hash_generation_current(t, nodebase)) {
	  tab_release_claimed_free(nodebase, n);
	  lj_gc2_smr_read_leave(g);
	  return -1;
	}
	setnilV(&expect);
	if (lj_tv_cas(&n->val, &expect, claim)) {
	  if (!tab_hash_generation_current(t, nodebase)) {
	    tab_release_claimed_free(nodebase, n);
	    lj_gc2_smr_read_leave(g);
	    return -1;
	  }
	  reserved = n;  /* Claimed free node; not visible until CAS-prepend. */
	  break;
	}
	lj_tab_storenilraw(&n->key);
      }
      if (!reserved) {
	lj_tab_node_free_release(nodebase);
	lj_gc2_smr_read_leave(g);
	return 0;  /* No free node in this hash generation: resize fallback. */
      }
      continue;  /* Re-scan chain before publishing the claimed node. */
    }
    if (LJ_UNLIKELY(anchor == NULL)) {
      tab_release_claimed_free(nodebase, reserved);
      lj_gc2_smr_read_leave(g);
      return 0;
    }
    if (!tab_hash_generation_current(t, nodebase)) {
      tab_release_claimed_free(nodebase, reserved);
      lj_gc2_smr_read_leave(g);
      return -1;
    }
    n = lj_tab_nextnode_acq(anchor);
    lj_tab_nextnode_set(reserved, n);
    if (tab_nextnode_cas(anchor, &n, reserved)) {
      tab_storekeyrel(L, &reserved->key, key);
      *slot = &reserved->val;
      lj_gc2_smr_read_leave(g);
      return 1;  /* 11.4 FINREG collision insert CAS-prepend. */
    }
    tab_release_claimed_free(nodebase, reserved);
    lj_gc2_smr_read_leave(g);
    lj_tab_wait_no_l();
    goto retry_attempt;
  found_existing:
    lj_tab_node_free_release(nodebase);
    lj_gc2_smr_read_leave(g);
    return -1;
  }
  }
}

TValue *lj_tab_setinth(lua_State *L, GCtab *t, int32_t key)
{
  TValue k;
  k.n = (lua_Number)key;
  for (;;) {
    TValue *slot;
    int status = tab_resolve_current_keyed_try(G(L), t, &k, &slot);
    if (status == TAB_RESOLVE_FOUND)
      return slot;
    if (status == TAB_RESOLVE_ABSENT)
      return lj_tab_newkey(L, t, &k);
    lj_tab_wait_l(L);
  }
}

TValue *lj_tab_setstr(lua_State *L, GCtab *t, const GCstr *key)
{
  TValue k;
  setstrV(L, &k, key);
  for (;;) {
    TValue *slot;
    int status = tab_resolve_current_keyed_try(G(L), t, &k, &slot);
    if (status == TAB_RESOLVE_FOUND)
      return slot;
    if (status == TAB_RESOLVE_ABSENT)
      return lj_tab_newkey(L, t, &k);
    /* k may be the only exact root for a freshly interned string. Anchor it
    ** before the first STOPREQ-visible retry. */
    return tab_set_anchored_key(L, t, &k);
  }
}

TValue *lj_tab_setint(lua_State *L, GCtab *t, int32_t key)
{
  TValue k;
  setintV(&k, key);
  for (;;) {
    TValue *slot;
    int status = tab_resolve_current_keyed_try(G(L), t, &k, &slot);
    if (status == TAB_RESOLVE_FOUND)
      return slot;
    if (status == TAB_RESOLVE_ABSENT)
      return lj_tab_setinth(L, t, key);  /* Canonical numeric hash key. */
    lj_tab_wait_l(L);
  }
}

TValue *lj_tab_set(lua_State *L, GCtab *t, cTValue *key)
{
  lj_tab_nomm_rel(t, 0);  /* Invalidate negative metamethod cache. */
  if (tvisstr(key)) {
    return lj_tab_setstr(L, t, strV(key));
  } else if (tvisint(key)) {
    return lj_tab_setint(L, t, intV(key));
  } else if (tvisnum(key)) {
    int64_t i64;
    int32_t k;
    if (lj_num2int_check(numV(key), i64, k))
      return lj_tab_setint(L, t, k);
    if (tvisnan(key))
      lj_err_msg(L, LJ_ERR_NANIDX);
    /* Else use the generic lookup. */
  } else if (tvisnil(key)) {
    lj_err_msg(L, LJ_ERR_NILIDX);
  }
  for (;;) {
    TValue *slot;
    int status = tab_resolve_current_keyed_try(G(L), t, key, &slot);
    if (status == TAB_RESOLVE_FOUND)
      return slot;
    if (status == TAB_RESOLVE_ABSENT)
      return lj_tab_newkey(L, t, key);
    /* Generic GC keys can exist only in a C local/TMPREF at this boundary. */
    return tab_set_anchored_key(L, t, key);
  }
}

LJ_FUNCA TValue *lj_tab_storetv(lua_State *L, TValue *dst, cTValue *src)
{
  copyTVrel(L, dst, src);
  return dst;
}

LJ_FUNCA void lj_tab_store_wait_l(lua_State *L)
{
  lj_tab_wait_l(L);
}

static LJ_AINLINE void tab_store_barrier_write(lua_State *L, GCtab *parent,
					       cTValue *key, cTValue *src)
{
  TValue snap;
  /*
  ** Helper-backed trace/VM stores may be the first C boundary after a fresh table
  ** allocation. The parent pointer is only a native argument at that point; root
  ** it before any weak-barrier, retry wait, resize, or GC work can run.
  */
  if (parent)
    lj_gc_pubobjroot(L, obj2gco(parent));
  /*
  ** Helper callers can pass keys that exist only as JIT TMPREFs or VM scratch
  ** TValues until the new slot is published. The slow missing-key path anchors
  ** keys before rehash, but weak barriers and current-generation retries happen
  ** before every store reaches that path. Publish the key here so fresh string
  ** keys cannot disappear while a traced store is still resolving the slot.
  */
  lj_gc_pubroot(L, key);
  lj_gc2_barrier_weak_write(L, parent, key, src);
  lj_gc_pubroot(L, src);
  /*
  ** Helper-backed JIT/VM stores can be the only publication point for a fresh
  ** trace value. Use the shared table-value publication wrapper so the value is
  ** visible to both GC2 and the incremental barrier; fresh-table traces
  ** do not always emit a separate TBAR after the helper call.
  */
  lj_gc_pubtabtv(L, parent, src);
  if (!src)
    return;
  lj_tv_load_acq(&snap, src);
  if (tvisgcv(&snap)) {
    tab_publish_storage(L, parent);
    lj_gc_pubtab(L, parent);
  }
}

static LJ_AINLINE void tab_publish_storage(lua_State *L, GCtab *t)
{
  global_State *g;
  LJGC2Lease lease;
  TValue *array;
  MSize asize, acap, hmask;
  Node *node;
  if (!L || !t)
    return;
  g = G(L);
  /* Retired-vector reclaim is opportunistic. A store publication must never
  ** wait for it: the table/value barriers below retain semantic work, while a
  ** successful tactical reader only marks the current side generations early. */
  if (!lj_gc2_smr_read_try(g))
    return;
  if (lj_gc2_obj_lease_acquire(g, obj2gco(t), (uint32_t)~LJ_TTAB,
				NULL, &lease) < 0) {
    lj_gc2_smr_read_leave(g);
    return;
  }
  /* A resize may retire either side vector immediately after its generation
  ** snapshot. Keep both snapshots and their physical mark publications in one
  ** SMR read interval, while lease keeps the table body admitted. Reclamation
  ** therefore cannot free either the table or a returned vector between
  ** snapshot validation and lj_gc2_markmem(). */
  if (lj_tab_array_snapshot_gc_held(g, t, &array, &asize, &acap) ==
      LJ_TAB_GC_SNAPSHOT_OK && array)
    (void)lj_gc2_markmem(g, acap ? (void *)lj_tab_array_hdrw(array) :
				   (void *)array);
  UNUSED(asize);
  if (lj_tab_node_snapshot_gc_held(g, t, &node, &hmask) ==
      LJ_TAB_GC_SNAPSHOT_OK && hmask > 0)
    (void)lj_gc2_markmem(g, lj_tab_node_hdrw(node));
  lj_gc2_lease_release(&lease);
  lj_gc2_smr_read_leave(g);
}

static LJ_AINLINE int tab_trystoretv_cas_once(lua_State *L, TValue *dst,
					      cTValue *src)
{
  TValue old;
  UNUSED(L);
  lj_tv_load_acq(&old, dst);
  if (tvisforward(&old))
    return LJ_TAB_STORE_CAS_FORWARD;
  if (lj_tv_cas(dst, &old, src))
    return LJ_TAB_STORE_CAS_OK;  /* 06 section 6.3.2: CAS-published store. */
  return tvisforward(&old) ? LJ_TAB_STORE_CAS_FORWARD :
	 LJ_TAB_STORE_CAS_CHANGED;
}

enum {
  TAB_STORE_MODE_ANY,
  TAB_STORE_MODE_EXISTING,
  TAB_STORE_MODE_NIL
};

static LJ_AINLINE int tab_trystoretv_cas_expected(TValue *dst,
						   TValue *expected,
						   cTValue *src,
						   int mode,
						   TValue *observed)
{
  if (tvisforward(expected))
    return LJ_TAB_STORE_CAS_FORWARD;
  if (tab_val_is_publish_claim(expected))
    return LJ_TAB_STORE_CAS_CHANGED;
  if (mode == TAB_STORE_MODE_NIL) {
    if (observed)
      *observed = *expected;
    if (!tvisnil(expected))
      return LJ_TAB_STORE_CAS_EXISTS;
  } else {
    if (mode == TAB_STORE_MODE_EXISTING && tvisnil(expected))
      return LJ_TAB_STORE_CAS_ABSENT;
  }
  if (lj_tv_cas(dst, expected, src))
    return LJ_TAB_STORE_CAS_OK;  /* 06 section 6.3.2: semantic store LP. */
  if (tvisforward(expected))
    return LJ_TAB_STORE_CAS_FORWARD;
  if (tab_val_is_publish_claim(expected))
    return LJ_TAB_STORE_CAS_CHANGED;
  if (mode == TAB_STORE_MODE_NIL) {
    if (observed)
      *observed = *expected;
    if (!tvisnil(expected))
      return LJ_TAB_STORE_CAS_EXISTS;
  } else {
    if (mode == TAB_STORE_MODE_EXISTING && tvisnil(expected))
      return LJ_TAB_STORE_CAS_ABSENT;
  }
  return LJ_TAB_STORE_CAS_CHANGED;
}

LJ_FUNCA int lj_tab_trystoretv_cas(lua_State *L, TValue *dst, cTValue *src)
{
  int rc;
  for (;;) {
    rc = tab_trystoretv_cas_once(L, dst, src);
    if (rc != LJ_TAB_STORE_CAS_CHANGED)
      return rc;
    lj_tab_store_wait_l(L);
  }
}

static LJ_AINLINE int tab_ptr_index(uintptr_t base, uintptr_t elem,
				    size_t elemsz, MSize count, MSize *idx)
{
  uintptr_t end = base + (uintptr_t)count * elemsz;
  if (elem >= base && elem < end && (elem - base) % elemsz == 0) {
    *idx = (MSize)((elem - base) / elemsz);
    return 1;
  }
  return 0;
}

static LJ_AINLINE MSize tab_store_array_snapshot_acq(GCtab *parent,
						      TValue **arrayp)
{
  for (;;) {
    MSize asize = lj_tab_asize_acq(parent);
    TValue *array = lj_tab_array_acq(parent);
    if (array && !lj_tab_array_is_colocated(parent, array)) {
      asize = lj_tab_array_hdr_asize_acq(array);
    } else {
      MSize asize2 = lj_tab_asize_acq(parent);
      if (asize2 != asize) {
	lj_tab_wait_no_l();
	continue;
      }
    }
    if (array == lj_tab_array_acq(parent)) {
      *arrayp = array;
      return asize;
    }
    lj_tab_wait_no_l();
  }
}

static LJ_AINLINE MSize tab_store_node_snapshot_acq(GCtab *parent,
						    Node **nodep)
{
  for (;;) {
    Node *node = lj_tab_node_acq(parent);
    MSize hmask = lj_tab_node_hmask_acq(node);
    if (node == lj_tab_node_acq(parent)) {
      *nodep = node;
      return hmask;
    }
    lj_tab_wait_no_l();
  }
}

static LJ_AINLINE int tab_array_forward_hop_writer(const GCtab *t,
						   TValue **arrayp,
						   MSize *asizep)
{
  TValue *array = *arrayp;
  TValue *root;
  TValue *next;
  if (!array || lj_tab_array_is_colocated(t, array))
    return 0;
  root = lj_tab_array_acq(t);
  if (root != array) {
    *asizep = lj_tab_array_snapshot_acq(t, arrayp);
    return *arrayp != NULL;
  }
  if (!lj_tab_array_is_retiring(t, array))
    return 0;
  next = lj_tab_array_nextgen_acq(array);
  if (next && next != array && !lj_tab_array_is_colocated(t, next)) {
    *arrayp = next;
    *asizep = lj_tab_array_hdr_asize_acq(next);
    return 1;
  }
  return 0;
}

static int tab_current_array_slot_for_key(GCtab *parent, TValue *dst,
					  int32_t key)
{
  TValue *array;
  MSize asize;
  if (key < 0)
    return 0;
  asize = tab_store_array_snapshot_acq(parent, &array);
  if (!array || (MSize)key >= asize)
    return 0;
  if (dst == &array[key] && !lj_tab_array_is_retiring(parent, array))
    return 1;
  {
    TValue val;
    TValue *next = array;
    MSize nextasize = asize;
    lj_tv_load_acq(&val, &array[key]);
    if ((lj_tab_array_is_retiring(parent, array) || tvisforward(&val)) &&
	(tvisforward(&val) ?
	 lj_tab_array_forward_hop_forward(parent, &next, &nextasize) :
	 tab_array_forward_hop_writer(parent, &next, &nextasize)) &&
	(MSize)key < nextasize)
      return dst == &next[key] && !lj_tab_array_is_retiring(parent, next);
  }
  return 0;
}

static int tab_current_hash_slot_for_key(GCtab *parent, TValue *dst,
					 cTValue *key)
{
  Node *node;
  MSize hmask = tab_store_node_snapshot_acq(parent, &node);
  MSize idx;
  TValue nk;
  if (lj_tab_node_is_retiring(node)) {
    node = lj_tab_node_nextgen_acq(node);
    if (!node)
      return 0;
    hmask = lj_tab_node_hmask_acq(node);
    if (lj_tab_node_is_retiring(node))
      return 0;
  }
  if (hmask == 0)
    return 0;
  if (!tab_ptr_index((uintptr_t)node, (uintptr_t)dst, sizeof(Node),
		     hmask + 1u, &idx) || dst != &node[idx].val) {
    Node *n = hashkey_node(node, hmask, key);
    do {
      lj_tv_load_acq(&nk, &n->key);
      if (!tab_key_islocked(&nk) && !tvisnil(&nk) && lj_obj_equal(&nk, key)) {
	TValue val;
	lj_tv_load_acq(&val, &n->val);
	if (tvisforward(&val)) {
	  Node *nextnode = node;
	  MSize nexthmask = hmask;
	  TValue *slot = tab_forwarded_setslot(parent, &nextnode, &nexthmask,
					       key);
	  return slot == dst;
	}
	return 0;
      }
    } while ((n = lj_tab_nextnode_acq(n)));
    return 0;
  }
  lj_tv_load_acq(&nk, &node[idx].key);
  return !tab_key_islocked(&nk) && !tvisnil(&nk) && lj_obj_equal(&nk, key);
}

static int tab_current_slot_for_key(GCtab *parent, TValue *dst, cTValue *key)
{
  int32_t k;
  int64_t i64;
  TValue hkey;
  cTValue *keyh = key;
  if (tvisint(key)) {
    k = intV(key);
    if (tab_current_array_slot_for_key(parent, dst, k))
      return 1;
    setnumV(&hkey, (lua_Number)k);
    keyh = &hkey;
  } else if (tvisnum(key) && lj_num2int_check(numV(key), i64, k)) {
    if (tab_current_array_slot_for_key(parent, dst, k))
      return 1;
  }
  return tab_current_hash_slot_for_key(parent, dst, keyh);
}

/* Single-attempt current-generation validation for descriptor-active stores.
** The caller holds GC2 SMR. A racing structural snapshot is reported stale;
** this routine never assists, waits or follows a retiring vector. */
static LJ_AINLINE int tab_store_array_snapshot_try(GCtab *parent,
						    TValue **arrayp,
						    MSize *asizep)
{
  MSize asize = lj_tab_asize_acq(parent);
  TValue *array = lj_tab_array_acq(parent);
  if (array && !lj_tab_array_is_colocated(parent, array)) {
    asize = lj_tab_array_hdr_asize_acq(array);
  } else if (lj_tab_asize_acq(parent) != asize) {
    return 0;
  }
  if (array != lj_tab_array_acq(parent))
    return 0;
  *arrayp = array;
  *asizep = asize;
  return 1;
}

static LJ_AINLINE int tab_store_node_snapshot_try(GCtab *parent,
						  Node **nodep,
						  MSize *hmaskp)
{
  Node *node = lj_tab_node_acq(parent);
  MSize hmask;
  if (!node)
    return 0;
  hmask = lj_tab_node_hmask_acq(node);
  if (node != lj_tab_node_acq(parent))
    return 0;
  *nodep = node;
  *hmaskp = hmask;
  return 1;
}

static TValue *tab_store_hash_slot_for_key_bounded(Node *node, MSize hmask,
						    cTValue *key)
{
  Node *n;
  MSize visited = 0;
  if (!node || hmask == 0)
    return NULL;
  n = hashkey_node(node, hmask, key);
  while (n) {
    TValue nk;
    Node *next;
    if (!tab_node_in_snapshot(node, hmask, n) || visited++ > hmask)
      return NULL;
    lj_tv_load_acq(&nk, &n->key);
    if (tab_key_islocked(&nk))
      return NULL;
    if (!tvisnil(&nk) && lj_obj_equal(&nk, key))
      return &n->val;
    next = lj_tab_nextnode_acq(n);
    if (next && !tab_node_in_snapshot(node, hmask, next))
      return NULL;
    n = next;
  }
  return NULL;
}

static int tab_store_array_successor_slot_bounded(GCtab *parent,
						   TValue *array,
						   MSize asize,
						   MSize key,
						   TValue *dst,
						   int *to_hash)
{
  TValue before, confirm;
  TValue *next;
  MSize nextasize;
  int have_old_slot = key < asize;
  if (to_hash)
    *to_hash = 0;
  if (!array || lj_tab_array_is_colocated(parent, array))
    return 0;
  /* A key outside this array comes from the old hash part. Its FORWARD marker
  ** precedes successor value installation, so neither RETIRING nor FORWARD can
  ** prove a delete/put-if-absent store is safe in the unpublished array. Wait
  ** for the new array root instead of inventing a cross-part handoff. */
  if (!have_old_slot)
    return 0;
  lj_tv_load_acq(&before, &array[key]);
  if (!tvisforward(&before) &&
      !lj_tab_array_is_retiring(parent, array))
    return 0;
  next = lj_tab_array_nextgen_acq(array);
  if (!next || next == array || lj_tab_array_is_colocated(parent, next))
    return 0;
  nextasize = lj_tab_array_hdr_asize_acq(next);
  if (lj_tab_array_is_retiring(parent, next) ||
      array != lj_tab_array_acq(parent) ||
      next != lj_tab_array_nextgen_acq(array))
    return 0;
  lj_tv_load_acq(&confirm, &array[key]);
  if (!tvisforward(&confirm) &&
      !lj_tab_array_is_retiring(parent, array))
    return 0;
  if (key < nextasize) {
    /* Even FORWARD is not a completion certificate: the resize owner may have
    ** captured the old value before an assist installed the successor, then
    ** resume after a delete and refill the nil destination. Until migration
    ** owns a unique persistent MOVING descriptor, same-index successor stores
    ** retry until the new array is the published table root. */
    UNUSED(dst);
    return 0;
  }
  /* Array shrink transfers this old integer slot to the successor hash part.
  ** RETIRING identifies the transfer, but the exact hash proof below must see
  ** the already-published non-retiring hash root before accepting dst. */
  if (to_hash)
    *to_hash = 1;
  return 0;
}

static int tab_current_slot_for_key_bounded(GCtab *parent, TValue *dst,
					     cTValue *key)
{
  int32_t k;
  int64_t i64;
  int intkey = 0;
  TValue hkey;
  cTValue *keyh = key;
  int array_to_hash = 0;
  if (tvisint(key)) {
    k = intV(key);
    intkey = 1;
    setnumV(&hkey, (lua_Number)k);
    keyh = &hkey;
  } else if (tvisnum(key) && lj_num2int_check(numV(key), i64, k)) {
    intkey = 1;
  }
  if (intkey && k >= 0) {
    TValue *array;
    MSize asize;
    if (!tab_store_array_snapshot_try(parent, &array, &asize))
      return 0;
    if (array) {
      if ((MSize)k < asize && dst == &array[k] &&
	  !lj_tab_array_is_retiring(parent, array) &&
	  array == lj_tab_array_acq(parent))
	return 1;
      if (tab_store_array_successor_slot_bounded(
	    parent, array, asize, (MSize)k, dst, &array_to_hash))
	return 1;
      if ((MSize)k < asize && !array_to_hash)
	return 0;
    }
  }
  {
    Node *node;
    MSize hmask;
    TValue *slot;
    if (!tab_store_node_snapshot_try(parent, &node, &hmask) || hmask == 0)
      return 0;
    slot = tab_store_hash_slot_for_key_bounded(node, hmask, keyh);
    if (slot == dst && !lj_tab_node_is_retiring(node) &&
	node == lj_tab_node_acq(parent))
      return 1;
    /* Hash FORWARD/RETIRING is published before the successor value. Exact
    ** next-generation key lookup therefore cannot distinguish an installed
    ** edge from the migration's transient nil slot. Wait for successor-root
    ** publication, which follows the complete hash copy, rather than allow a
    ** delete or NIL-mode claim to be resurrected by put-if-absent migration. */
    return 0;
  }
}

LJ_FUNCA int lj_tab_read_current_keyed(global_State *g, GCtab *parent,
				       TValue *dst, cTValue *key, TValue *oldp)
{
  int rc = LJ_TAB_STORE_CAS_STALE;
  if (!g || !parent || !dst || !key || !oldp || !lj_gc2_smr_read_try(g))
    return rc;
  /* dst was resolved before this call. Validate it without dereferencing,
  ** then keep the matching vector generation alive through load and the
  ** closing currentness proof. */
  if (tab_current_slot_for_key_bounded(parent, dst, key)) {
    lj_tv_load_acq(oldp, dst);
    if (tvisforward(oldp))
      rc = LJ_TAB_STORE_CAS_FORWARD;
    else if (tab_current_slot_for_key_bounded(parent, dst, key))
      rc = LJ_TAB_STORE_CAS_OK;
  }
  lj_gc2_smr_read_leave(g);
  return rc;
}

static LJ_AINLINE int tab_store_guard_needed(cTValue *key, cTValue *src)
{
  return tvisgcv(src) || (tvisgcv(key) && !tvisnil(src));
}

static void tab_store_guard_finish_checked(lua_State *L,
					    LJGC2TableStoreGuard *guard,
					    int committed)
{
  (void)lj_gc2_table_store_finish(L, guard, committed);
  if (LJ_UNLIKELY(guard->cleanup_failed || !guard->finished)) {
    /* Any unfinished lifecycle leaves a descriptor, lease, weak token or exact
    ** registry borrow represented only by this stack guard. Global pins are
    ** fail-closed, but continuation would discard the owning representation. */
    lj_assertL(0, "table-store guard lost cleanup authority");
    abort();
  }
}

static int tab_trystoretv_cas_keyed_once_mode(lua_State *L,
					       GCtab *parent, TValue *dst,
					       cTValue *key, cTValue *src,
					       int mode, TValue *observed)
{
  global_State *g;
  TValue keysnap, valuesnap, expected;
  int committed = 0;
  int guarded;
  int rc = LJ_TAB_STORE_CAS_STALE;
  if (!parent)
    return tab_trystoretv_cas_once(L, dst, src);
  lj_tv_load_acq(&keysnap, key);
  lj_tv_load_acq(&valuesnap, src);
  guarded = tab_store_guard_needed(&keysnap, &valuesnap);
  /* Trace/VM scratch operands must become semantic roots before SMR failure,
  ** retry, or descriptor publication can make their native locations stale. */
  if (guarded)
    tab_store_barrier_write(L, parent, &keysnap, &valuesnap);
  /*
  ** Keyed CAS validates the slot before and after the store. Hold a short SMR
  ** reader across that validation/CAS window so the resolved table generation
  ** cannot be reclaimed between the current-slot proof and the slot load.
  ** Every validation below is single-attempt: no descriptor-active path waits,
  ** assists a resize, allocates, throws, or performs an L-aware callout.
  */
  g = L ? G(L) : NULL;
  /* Failure to enter is a generation-stale result, not a reason to wait for
  ** the opportunistic reclaimer. The keyed caller re-resolves from the table
  ** root and retries without retaining this raw slot. */
  if (!lj_gc2_smr_read_try(g))
    return LJ_TAB_STORE_CAS_STALE;
  if (!tab_current_slot_for_key_bounded(parent, dst, &keysnap)) {
    /* dst was resolved before this SMR interval and can already name a
    ** reclaimed generation. Failed currentness is the complete answer: never
    ** inspect the rejected raw pointer to refine EXISTING into ABSENT/FORWARD.
    ** The caller re-resolves from the table root outside this interval. */
    goto leave;
  }
  lj_tv_load_acq(&expected, dst);
  if (tvisforward(&expected)) {
    rc = LJ_TAB_STORE_CAS_FORWARD;
    goto leave;
  }
  if (tab_val_is_publish_claim(&expected)) {
    rc = LJ_TAB_STORE_CAS_CHANGED;
    goto leave;
  } else if (mode == TAB_STORE_MODE_NIL) {
    if (observed)
      *observed = expected;
    if (!tvisnil(&expected)) {
      rc = LJ_TAB_STORE_CAS_EXISTS;
      goto leave;
    }
  } else if (mode == TAB_STORE_MODE_EXISTING && tvisnil(&expected)) {
    rc = LJ_TAB_STORE_CAS_ABSENT;
    goto leave;
  }
  if (guarded) {
    LJGC2TableStoreGuard guard;
    LJGC2TableStoreGuardResult stage;
    stage = lj_gc2_table_store_begin(L, &guard, parent,
				     &keysnap, &valuesnap);
    if (stage != LJ_GC2_TABLE_STORE_GUARD_OK &&
	stage != LJ_GC2_TABLE_STORE_GUARD_PINNED) {
      if (!guard.finished && guard.g && guard.tg)
	tab_store_guard_finish_checked(L, &guard, 0);
      goto leave;
    }
    stage = lj_gc2_table_store_admit(L, &guard);
    if ((stage != LJ_GC2_TABLE_STORE_GUARD_OK &&
	 stage != LJ_GC2_TABLE_STORE_GUARD_PINNED) ||
	!guard.gate_admitted)
      goto guarded_finish;
    if (!tab_current_slot_for_key_bounded(parent, dst, &guard.key))
      goto guarded_finish;
    stage = lj_gc2_table_store_revalidate(L, &guard);
    if ((stage != LJ_GC2_TABLE_STORE_GUARD_OK &&
	 stage != LJ_GC2_TABLE_STORE_GUARD_PINNED) ||
	!guard.store_authorized)
      goto guarded_finish;
    /* Final revalidation is immediately adjacent to the semantic CAS. The
    ** expected old value and by-value guard payload are the only operands. */
    rc = tab_trystoretv_cas_expected(dst, &expected, &guard.value,
					     mode, observed);
    committed = rc == LJ_TAB_STORE_CAS_OK;
    if (committed)
      tab_test_store_post_cas(L, parent, dst, &guard.key, &guard.value);
    if (committed &&
	!tab_current_slot_for_key_bounded(parent, dst, &guard.key))
      rc = LJ_TAB_STORE_CAS_STALE;
guarded_finish:
    /* A committed CAS must hand off even when post-currentness returns STALE.
    ** Every other exit clears ACTIVE before the caller may wait or retry. */
    tab_store_guard_finish_checked(L, &guard, committed);
  } else {
    rc = tab_trystoretv_cas_expected(dst, &expected, &valuesnap,
					     mode, observed);
    if (rc == LJ_TAB_STORE_CAS_OK &&
	!tab_current_slot_for_key_bounded(parent, dst, &keysnap)) {
      lj_gc_pubtabtv(L, parent, &valuesnap);
      rc = LJ_TAB_STORE_CAS_STALE;
    }
  }
leave:
  lj_gc2_smr_read_leave(g);
  return rc;
}

static LJ_AINLINE int tab_trystoretv_cas_keyed_once(lua_State *L,
						    GCtab *parent,
						    TValue *dst,
						    cTValue *key,
						    cTValue *src)
{
  return tab_trystoretv_cas_keyed_once_mode(
    L, parent, dst, key, src, TAB_STORE_MODE_ANY, NULL);
}

LJ_FUNCA int lj_tab_trystoretv_cas_keyed(lua_State *L, GCtab *parent,
					 TValue *dst, cTValue *key,
					 cTValue *src)
{
  int rc;
  for (;;) {
    rc = tab_trystoretv_cas_keyed_once(L, parent, dst, key, src);
    if (rc != LJ_TAB_STORE_CAS_CHANGED)
      return rc;
    lj_tab_store_wait_l(L);
  }
}

static int tab_trystoretv_cas_keyed_weak(lua_State *L, GCtab *parent,
					 TValue *dst, cTValue *key,
					 cTValue *src)
{
  int rc, weakwr;
  /*
  ** The weak-write counter only needs to cover the edge publication and the
  ** matching weak barrier. Slot resolution, resize assist and retry waits can
  ** allocate or observe STOPREQ, so they must run before/after this window. The
  ** one-shot CAS helper below only uses non-throwing current-slot validation.
  */
  weakwr = lj_gc2_weak_write_begin(L, parent);
  if (weakwr)
    tab_store_barrier_write(L, parent, key, src);
  rc = tab_trystoretv_cas_keyed_once(L, parent, dst, key, src);
  if (weakwr) {
    if (rc == LJ_TAB_STORE_CAS_OK || rc == LJ_TAB_STORE_CAS_STALE)
      tab_store_barrier_write(L, parent, key, src);
    lj_gc2_weak_write_end(L, weakwr);
  }
  return rc;
}

static int tab_clear_weak_slot_rawkey(TValue *dst, cTValue *key, cTValue *val)
{
  Node *n;
  TValue old, expect, curkey, nilv;
  if (!dst || !key || !val || !tvisgcv(key) || tvisstr(key))
    return LJ_TAB_STORE_CAS_STALE;
  n = (Node *)(void *)dst;  /* Node.val is the first field. */
  setnilV(&nilv);
  for (;;) {
    /*
    ** Weak-key clearing often runs after the key object is already dead. The
    ** table intentionally keeps the stale raw key bits until resize, so a normal
    ** keyed lookup may be unable to hash or compare the key safely. The weak
    ** scanner already owns a concrete hash slot snapshot; compare that slot's raw
    ** key and value words and clear only that exact stale entry.
    */
    lj_tv_load_acq(&curkey, &n->key);
    if (tv_rawload(&curkey) != tv_rawload(key))
      return LJ_TAB_STORE_CAS_STALE;
    lj_tv_load_acq(&old, dst);
    if (tvisnil(&old))
      return LJ_TAB_STORE_CAS_OK;
    if (tv_rawload(&old) != tv_rawload(val))
      return LJ_TAB_STORE_CAS_CHANGED;
    expect = old;
    if (lj_tv_cas(dst, &expect, &nilv)) {
      lj_tv_load_acq(&curkey, &n->key);
      return tv_rawload(&curkey) == tv_rawload(key) ?
	     LJ_TAB_STORE_CAS_OK : LJ_TAB_STORE_CAS_STALE;
    }
    if (tvisforward(&expect))
      return LJ_TAB_STORE_CAS_FORWARD;
    if (tv_rawload(&expect) != tv_rawload(val))
      return LJ_TAB_STORE_CAS_CHANGED;
    lj_tab_wait_no_l();
  }
}

LJ_FUNCA int lj_tab_trysetnil_cas_keyed(lua_State *L, GCtab *parent,
					TValue *dst, cTValue *key,
					cTValue *src, TValue *oldp)
{
  int rc;
  for (;;) {
    rc = tab_trystoretv_cas_keyed_once_mode(
      L, parent, dst, key, src, TAB_STORE_MODE_NIL, oldp);
    if (rc != LJ_TAB_STORE_CAS_CHANGED)
      return rc;
    lj_tab_store_wait_l(L);
  }
}

int lj_tab_clear_weak_slot_keyed(global_State *g, GCtab *parent, TValue *dst,
				 cTValue *key, cTValue *val)
{
  TValue old, expect, nilv;
  setnilV(&nilv);
  for (;;) {
    int rc = lj_tab_read_current_keyed(g, parent, dst, key, &old);
    if (rc != LJ_TAB_STORE_CAS_OK) {
      if (rc == LJ_TAB_STORE_CAS_STALE)
	return tab_clear_weak_slot_rawkey(dst, key, val);
      return rc;
    }
    if (tvisnil(&old))
      return LJ_TAB_STORE_CAS_OK;
    if (tv_rawload(&old) != tv_rawload(val))
      return LJ_TAB_STORE_CAS_CHANGED;
    expect = old;
    if (lj_tv_cas(dst, &expect, &nilv))
      return tab_current_slot_for_key(parent, dst, key) ?
	     LJ_TAB_STORE_CAS_OK : LJ_TAB_STORE_CAS_STALE;
    if (tvisforward(&expect))
      return LJ_TAB_STORE_CAS_FORWARD;
    if (tv_rawload(&expect) != tv_rawload(val))
      return LJ_TAB_STORE_CAS_CHANGED;
    lj_tab_wait_no_l();
  }
}

static LJ_AINLINE void tab_clear_store_wait(lua_State *L, int guarded)
{
  /*
  ** A fresh STOPREQ-visible wait can longjmp. While table.clear owns the
  ** per-table structural slot, retry waits must only yield to competing
  ** publishers and then unwind through the normal leave path below.
  */
  if (guarded)
    lj_tab_wait_no_l();
  else
    lj_tab_store_wait_l(L);
}

static LJ_AINLINE void tab_clear_wait(lua_State *L, int guarded)
{
  if (guarded)
    lj_tab_wait_no_l();
  else
    lj_tab_wait_l(L);
}

static int tab_clear_try_nil_keyed(lua_State *L, GCtab *parent, TValue *dst,
				   cTValue *key, int guarded)
{
  TValue old, expect, nilv;
  setnilV(&nilv);
  for (;;) {
    int rc = lj_tab_read_current_keyed(G(L), parent, dst, key, &old);
    if (rc != LJ_TAB_STORE_CAS_OK)
      return 0;
    if (tvisnil(&old))
      return 1;
    if (tab_val_is_publish_claim(&old)) {
      tab_clear_wait(L, guarded);
      continue;
    }
    expect = old;
    if (lj_tv_cas(dst, &expect, &nilv))
      return tab_current_slot_for_key(parent, dst, key);
    if (tvisforward(&expect))
      return 0;
    if (tab_val_is_publish_claim(&expect)) {
      tab_clear_wait(L, guarded);
      continue;
    }
    tab_clear_store_wait(L, guarded);
  }
}

static void tab_clear_array_shared(lua_State *L, GCtab *t, TValue *array,
				   MSize asize, int guarded)
{
  MSize i;
  for (i = 0; i < asize; i++) {
    TValue key;
    if (i > (MSize)INT32_MAX)
      break;
    setintV(&key, (int32_t)i);
    while (!tab_clear_try_nil_keyed(L, t, &array[i], &key, guarded))
      tab_clear_store_wait(L, guarded);
  }
}

static void tab_clear_hash_slot_shared(lua_State *L, GCtab *t, Node *n,
				       int guarded)
{
  for (;;) {
    TValue key, val;
    lj_tv_load_acq(&val, &n->val);
    if (tab_val_is_publish_claim(&val)) {
      tab_clear_wait(L, guarded);
      continue;
    }
    if (tab_val_absent(&val))
      return;
    lj_tv_load_acq(&key, &n->key);
    if (tab_key_islocked(&key)) {
      tab_clear_wait(L, guarded);
      continue;
    }
    if (tab_hash_key_hidden(&key))
      return;
    (void)tab_clear_try_nil_keyed(L, t, &n->val, &key, guarded);
    return;
  }
}

static void tab_clear_shared(lua_State *L, GCtab *t)
{
  TValue *array;
  Node *node;
  MSize asize, hmask, i;
  int guard = lj_tab_struct_enter(L, t);
  tab_test_clear_shared_call();
  asize = lj_tab_array_snapshot_acq(t, &array);
  if (array)
    tab_clear_array_shared(L, t, array, asize, guard);
  node = lj_tab_node_snapshot_acq(t, &hmask);
  if (hmask > 0) {
    for (i = 0; i <= hmask; i++)
      tab_clear_hash_slot_shared(L, t, &node[i], guard);
  }
  lj_tab_struct_leave(t, guard);
}

/* Clear a table. */
void LJ_FASTCALL lj_tab_clear(lua_State *L, GCtab *t)
{
  /*
  ** mt_entering has not published mt_active yet, but a secondary VM entry is
  ** already in progress. Avoid private raw clearing while the attach/spawn
  ** handoff can make the table visible to another thread.
  */
  if (mt_active_or_entering_acq(G(L)))
    tab_clear_shared(L, t);
  else
    tab_clear_raw(t);
}

LJ_FUNCA int32_t lj_tab_storetv_existing_forjit(lua_State *L, GCtab *parent,
						cTValue *key, cTValue *src)
{
  /*
  ** Traced values may still live only in registers/spill slots when a
  ** concurrent GC phase is active. Mark the source before any table retry can
  ** wait, resize, or otherwise give GC a chance to observe the world without
  ** this edge.
  */
  tab_store_barrier_write(L, parent, key, src);
  for (;;) {
    TValue *dst;
    int rc, status;
    status = tab_resolve_current_keyed_try(G(L), parent, key, &dst);
    if (status == TAB_RESOLVE_ABSENT)
      return 0;
    if (status == TAB_RESOLVE_RETRY) {
      lj_tab_store_wait_l(L);
      continue;
    }
    rc = tab_trystoretv_cas_keyed_once_mode(
      L, parent, dst, key, src, TAB_STORE_MODE_EXISTING, NULL);
    if (rc == LJ_TAB_STORE_CAS_OK) {
      tab_store_barrier_write(L, parent, key, src);
      return 1;
    }
    if (rc == LJ_TAB_STORE_CAS_ABSENT)
      return 0;
    /* FORWARD/STALE/CHANGED and a publish-claim collision are retried only
    ** after the guarded core has finished every descriptor/lease authority. */
    lj_tab_store_wait_l(L);
  }
}

#ifdef LJ_TAB_TEST_HELPERS
static TValue *tab_forwarded_jit_array_slot(lua_State *L, GCtab *parent,
					    TValue *array, MSize asize,
					    TValue *dst, MSize key)
{
  for (;;) {
    TValue val;
    MSize idx;
    int retiring = lj_tab_array_is_retiring(parent, array);
    lj_tv_load_acq(&val, dst);
    if (!tvisforward(&val) && !retiring)
      return dst;
    if (tvisforward(&val) && lj_tab_array_is_colocated(parent, array))
      return lj_tab_setint(L, parent, (int32_t)key);
    if (!tab_ptr_index((uintptr_t)array, (uintptr_t)dst, sizeof(TValue),
		       asize, &idx))
      return dst;
    /* A separated-array assist installs/preserves the successor value before
    ** publishing FORWARD. RETIRING alone precedes owner copy and cannot
    ** authorize a delete or NIL-mode store in the successor. */
    if (tvisforward(&val) &&
	lj_tab_array_forward_hop_forward(parent, &array, &asize)) {
      if (idx < asize)
	return &array[idx];
      return lj_tab_setinth(L, parent, (int32_t)key);
    }
    if (retiring) {
      TValue *slot = tab_resize_assist_array_slot(L, parent, array, asize, idx);
      if (slot)
	return slot;
    }
    if (retiring && lj_tab_array_acq(parent) == array) {
      lj_tab_store_wait_l(L);
      continue;
    }
    return lj_tab_setinth(L, parent, (int32_t)key);
  }
}

static TValue *tab_forwarded_jit_hash_slot(lua_State *L, GCtab *parent,
					   Node *node, MSize hmask, TValue *dst,
					   TValue *keycopy, cTValue **keyp,
					   cTValue *fallback_key)
{
  TValue val;
  Node *n;
  MSize idx;
  lj_tv_load_acq(&val, dst);
  if (!tvisforward(&val) && !lj_tab_node_is_retiring(node))
    return dst;
  if (!tab_ptr_index((uintptr_t)node, (uintptr_t)dst, sizeof(Node),
		     hmask + 1u, &idx))
    return dst;
  n = &node[idx];
  lj_tv_load_acq(keycopy, &n->key);
  if (tab_key_islocked(keycopy) || tvisnil(keycopy) ||
      !tab_tv_snapshot_valid(keycopy) || tab_hash_key_hidden(keycopy)) {
    if (fallback_key) {
      *keyp = fallback_key;
      return lj_tab_set(L, parent, fallback_key);
    }
    return dst;
  }
  *keyp = keycopy;
  {
    TValue *slot = tab_forwarded_setslot(parent, &node, &hmask, keycopy);
    return slot ? slot : lj_tab_set(L, parent, keycopy);
  }
}
#endif

static TValue *tab_current_jit_hash_slot(lua_State *L, GCtab *parent,
					 TValue *orig, cTValue *key,
					 TValue *keycopy, cTValue **keyp)
{
  UNUSED(orig);
  UNUSED(keycopy);
  *keyp = key;
  for (;;) {
    TValue *slot;
    int status = tab_resolve_current_keyed_try(G(L), parent, key, &slot);
    if (status == TAB_RESOLVE_FOUND)
      return slot;
    if (status == TAB_RESOLVE_ABSENT)
      return lj_tab_set(L, parent, key);
    /* No raw generation survives this wait. */
    lj_tab_store_wait_l(L);
  }
}

static TValue *tab_current_keyed_slot(lua_State *L, GCtab *parent,
				      cTValue *key)
{
  for (;;) {
    TValue *slot;
    int status = tab_resolve_current_keyed_try(G(L), parent, key, &slot);
    if (status == TAB_RESOLVE_FOUND)
      return slot;
    if (status == TAB_RESOLVE_ABSENT)
      return lj_tab_set(L, parent, key);
    /* Retry only after the bounded SMR attempt has released all vectors. */
      lj_tab_store_wait_l(L);
  }
}

#ifdef LJ_TAB_TEST_HELPERS
LJ_FUNCA TValue *lj_tab_test_storetv_forjit_array_observed(lua_State *L,
							   GCtab *parent,
							   TValue *array,
							   MSize asize,
							   TValue *dst,
							   cTValue *src,
							   MSize key)
{
  TValue *slot = tab_forwarded_jit_array_slot(L, parent, array, asize, dst,
					      key);
  TValue keytv;
  setintV(&keytv, (int32_t)key);
  if (slot == dst)
    return slot;
  return lj_tab_trystoretv_cas_keyed(L, parent, slot, &keytv, src) ==
	 LJ_TAB_STORE_CAS_OK ? slot : NULL;
}

LJ_FUNCA TValue *lj_tab_test_storetv_forjit_hash_observed(lua_State *L,
							  GCtab *parent,
							  Node *node,
							  MSize hmask,
							  TValue *dst,
							  cTValue *src)
{
  TValue keycopy;
  cTValue *barrier_key = NULL;
  TValue *slot = tab_forwarded_jit_hash_slot(L, parent, node, hmask, dst,
					     &keycopy, &barrier_key, NULL);
  if (slot == dst)
    return slot;
  return lj_tab_trystoretv_cas_keyed(L, parent, slot, barrier_key, src) ==
	 LJ_TAB_STORE_CAS_OK ? slot : NULL;
}
#endif

static TValue *tab_current_vm_array_key_slot(lua_State *L, GCtab *parent,
					     MSize key);

static TValue *tab_current_jit_array_slot(lua_State *L, GCtab *parent,
					  TValue *orig, MSize key)
{
  TValue keytv;
  UNUSED(orig);
  setintV(&keytv, (int32_t)key);
  return tab_current_keyed_slot(L, parent, &keytv);
}

LJ_FUNCA TValue *lj_tab_storetv_forjit_array_nogc(lua_State *L,
						  GCtab *parent,
						  TValue *dst, cTValue *src,
						  MSize key)
{
  TValue *orig = dst;
  TValue keytv;
  setintV(&keytv, (int32_t)key);
  for (;;) {
    dst = tab_current_jit_array_slot(L, parent, orig, key);
    if (lj_tab_trystoretv_cas_keyed(L, parent, dst, &keytv, src) ==
	LJ_TAB_STORE_CAS_OK)
      break;
    lj_tab_store_wait_l(L);  /* JIT array store saw stale/FORWARD slot. */
  }
  return dst;
}

LJ_FUNCA TValue *lj_tab_storetv_forjit_array(lua_State *L, GCtab *parent,
					     TValue *dst, cTValue *src,
					     MSize key)
{
  TValue *orig = dst;
  TValue keytv;
  setintV(&keytv, (int32_t)key);
  /*
  ** The source can be trace-only until this helper publishes it. Barrier it
  ** before any stale-slot retry path can yield to concurrent GC work.
  */
  tab_store_barrier_write(L, parent, &keytv, src);
  for (;;) {
    /*
    ** The recorded slot is a hint only. A resize can retire that generation
    ** before this helper runs, so every publish resolves the key through the
    ** current generation and lets the keyed CAS own the final validation.
    */
    dst = tab_current_jit_array_slot(L, parent, orig, key);
    if (tab_trystoretv_cas_keyed_weak(L, parent, dst, &keytv, src) ==
	LJ_TAB_STORE_CAS_OK)
      break;
    lj_tab_store_wait_l(L);  /* JIT array store saw stale/FORWARD slot. */
  }
  /*
  ** A resize can forward dst immediately after the CAS-published store. The
  ** source TValue is the stable edge the trace just wrote; barrier that
  ** snapshot rather than rereading a potentially stale slot.
  */
  tab_store_barrier_write(L, parent, NULL, src);
  return dst;
}

LJ_FUNCA TValue *lj_tab_storetv_forvm_array(lua_State *L, GCtab *parent,
					    TValue *dst, cTValue *src,
					    MSize key)
{
  TValue *orig = dst;
  TValue keytv;
  int stack_src = tab_key_on_stack(L, src);
  ptrdiff_t srcofs = stack_src ? savestack(L, (TValue *)(void *)src) : 0;
  UNUSED(orig);
  tab_test_vm_array_store_call();
  setintV(&keytv, (int32_t)key);
  if (stack_src) src = restorestack(L, srcofs);
  /*
  ** Active-MT VM stores publish with CAS and run the VM table barrier
  ** only after the helper returns. Publish the source before the CAS so a
  ** concurrent collector cannot observe the new slot without its value edge.
  */
  tab_store_barrier_write(L, parent, &keytv, src);
  for (;;) {
    if (stack_src) src = restorestack(L, srcofs);
    dst = tab_current_vm_array_key_slot(L, parent, key);
    if (stack_src) src = restorestack(L, srcofs);
    if (tab_trystoretv_cas_keyed_weak(L, parent, dst, &keytv, src) ==
	LJ_TAB_STORE_CAS_OK)
      break;
    lj_tab_store_wait_l(L);  /* VM array store saw stale/FORWARD slot. */
  }
  return dst;
}

LJ_FUNCA TValue *lj_tab_storetv_forvm_strhash(lua_State *L, GCtab *parent,
					      TValue *dst, cTValue *src,
					      GCstr *key)
{
  TValue keytv;
  TValue *orig = dst;
  int stack_src = tab_key_on_stack(L, src);
  ptrdiff_t srcofs = stack_src ? savestack(L, (TValue *)(void *)src) : 0;
  UNUSED(orig);
  tab_test_vm_strhash_store_call();
  setstrV(L, &keytv, key);
  if (stack_src) src = restorestack(L, srcofs);
  /*
  ** The x64 VM reaches this helper when direct stores are unsafe. Mirror the
  ** traced-store rule: publish the source before any CAS or retry can expose a
  ** slot update to concurrent GC.
  */
  tab_store_barrier_write(L, parent, &keytv, src);
  for (;;) {
    if (stack_src) src = restorestack(L, srcofs);
    dst = lj_tab_setstr(L, parent, key);
    if (stack_src) src = restorestack(L, srcofs);
    if (tab_trystoretv_cas_keyed_weak(L, parent, dst, &keytv, src) ==
	LJ_TAB_STORE_CAS_OK)
      break;
    lj_tab_store_wait_l(L);  /* VM hash store saw stale/FORWARD slot. */
  }
  return dst;
}

LJ_FUNCA TValue *lj_tab_storetv_forjit_hash(lua_State *L, GCtab *parent,
					    TValue *dst, cTValue *src,
					    cTValue *key)
{
  TValue *orig = dst;
  TValue keycopy;
  cTValue *barrier_key = key;
  /*
  ** A traced source may be invisible to the stack scanner until the helper
  ** completes. Publish the GC edge before any retry wait or resize can run.
  */
  tab_store_barrier_write(L, parent, key, src);
  for (;;) {
    /*
    ** Treat the traced slot as a location hint, not as authority. Hash resizes
    ** can move the key before generated code reaches this helper.
    */
    barrier_key = key;
    dst = tab_current_jit_hash_slot(L, parent, orig, key, &keycopy,
				    &barrier_key);
    if (tab_trystoretv_cas_keyed_weak(L, parent, dst, barrier_key, src) ==
	LJ_TAB_STORE_CAS_OK)
      break;
    lj_tab_store_wait_l(L);  /* JIT hash store saw stale/FORWARD slot. */
  }
  tab_store_barrier_write(L, parent, barrier_key, src);
  return dst;
}

LJ_FUNCA TValue *lj_tab_storetv_forjit_newref(lua_State *L, GCtab *parent,
					      TValue *dst, cTValue *src,
					      cTValue *key)
{
  /*
  ** NEWREF commonly stores freshly allocated trace values. Mark src before
  ** insertion retries, since the value may not be visible from an interpreter
  ** frame until this helper returns.
  */
  tab_store_barrier_write(L, parent, key, src);
  for (;;) {
    dst = lj_tab_set(L, parent, key);
    if (tab_trystoretv_cas_keyed_weak(L, parent, dst, key, src) ==
	LJ_TAB_STORE_CAS_OK)
      break;
    lj_tab_store_wait_l(L);  /* JIT NEWREF store saw stale/FORWARD slot. */
  }
  tab_store_barrier_write(L, parent, key, src);
  return dst;
}

LJ_FUNCA TValue *lj_tab_storetvn(lua_State *L, TValue *dst, cTValue *src,
				 uint32_t n)
{
  uint32_t i;
  for (i = 0; i < n; i++)
    copyTVrel(L, &dst[i], &src[i]);
  return dst;
}

static int tab_tsetm_barrier_needed(lua_State *L, GCtab *parent)
{
  TGState *tg;
  if (!L || !parent)
    return 0;
  tg = L2TG(L);
  return (tg && lj_tg_mark_active_acq(tg)) || isblack(obj2gco(parent));
}

static TValue *tab_current_vm_array_key_slot(lua_State *L, GCtab *parent,
					     MSize key)
{
  TValue keytv;
  setintV(&keytv, (int32_t)key);
  return tab_current_keyed_slot(L, parent, &keytv);
}

static void tab_tsetm_barrier_range(lua_State *L, GCtab *parent,
				    cTValue *src, uint32_t n)
{
  global_State *g = G(L);
  uint32_t i;
  for (i = 0; i < n; i++) {
    TValue value;
    /* Keyed stores already released their vector SMR interval. Barrier the
    ** stable source edge, never re-resolve and dereference a raw destination
    ** which a concurrent resize may have retired. */
    lj_tv_load_acq(&value, &src[i]);
    lj_gc2_barrier_tv_pair_g(g, obj2gco(parent), &value);
  }
  lj_gc2_barrier_tab(L, parent);  /* Preserve the previous TSETM table barrier. */
}

static LJ_AINLINE TValue *tab_tsetm_fast_range(GCtab *parent, uint32_t start,
					       uint32_t n)
{
  TValue *array;
  MSize asize;
  uint32_t i;
  if (n == 0)
    return NULL;
  asize = tab_store_array_snapshot_acq(parent, &array);
  if (!array || start > asize || n > asize - start)
    return NULL;
  if (!lj_tab_array_is_colocated(parent, array) &&
      lj_tab_array_is_retiring(parent, array))
    return NULL;
  for (i = 0; i < n; i++) {
    TValue old;
    lj_tv_load_acq(&old, &array[start + i]);
    if (tvisforward(&old))
      return NULL;
  }
  return &array[start];
}

LJ_FUNCA void lj_tab_storetvn_forvm_array(lua_State *L, GCtab *parent,
					  uint32_t start, cTValue *src,
					  uint32_t n)
{
  global_State *g;
  uint32_t i;
  int weakcand;
  int stack_src;
  ptrdiff_t srcofs;
  if (!L || !parent || !src || n == 0)
    return;
  g = G(L);
  stack_src = tab_key_on_stack(L, src);
  srcofs = stack_src ? savestack(L, (TValue *)(void *)src) : 0;
  weakcand = lj_gc2_weak_write_candidate(L, parent);
  if (stack_src) src = restorestack(L, srcofs);
  if (!weakcand)
    lj_gc2_barrier_tvn_pair_g(g, obj2gco(parent), src, n);
  if (stack_src) src = restorestack(L, srcofs);
  if (!weakcand && tab_private_mutation_allowed(L)) {
    TValue *dst = tab_tsetm_fast_range(parent, start, n);
    if (dst) {
      tab_test_tsetm_fast_call();
      (void)lj_tab_storetvn(L, dst, src, n);
      if (tab_tsetm_barrier_needed(L, parent))
	lj_gc_pubtabtvn_vm(L, parent, dst, n);
      return;
    }
  }
  for (i = 0; i < n; i++) {
    TValue *dst;
    TValue key;
    if (stack_src) src = restorestack(L, srcofs);
    setintV(&key, (int32_t)(start + i));
    if (weakcand)
      tab_store_barrier_write(L, parent, &key, &src[i]);
    for (;;) {
      dst = tab_current_vm_array_key_slot(L, parent, (MSize)(start + i));
      if (stack_src) src = restorestack(L, srcofs);
      if (tab_trystoretv_cas_keyed_weak(L, parent, dst, &key, &src[i]) ==
	  LJ_TAB_STORE_CAS_OK)
	break;
      lj_tab_store_wait_l(L);  /* VM TSETM saw stale/FORWARD slot. */
      if (stack_src) src = restorestack(L, srcofs);
    }
  }
  if (weakcand || tab_tsetm_barrier_needed(L, parent)) {
    if (stack_src) src = restorestack(L, srcofs);
    tab_tsetm_barrier_range(L, parent, src, n);
  }
}

TValue *lj_tab_storenilraw(TValue *dst)
{
  TValue tv;
  setnilV(&tv);
  tv_rawstore_rel(dst, tv_rawload(&tv));
  return dst;
}

TValue *lj_tab_storenil(lua_State *L, TValue *dst)
{
  UNUSED(L);
  return lj_tab_storenilraw(dst);
}

TValue *lj_tab_storebool(lua_State *L, TValue *dst, int b)
{
  TValue tv;
  setboolV(&tv, b);
  return lj_tab_storetv(L, dst, &tv);
}

TValue *lj_tab_storeint(lua_State *L, TValue *dst, int32_t i)
{
  TValue tv;
  setintV(&tv, i);
  return lj_tab_storetv(L, dst, &tv);
}

TValue *lj_tab_storeintptr(lua_State *L, TValue *dst, intptr_t i)
{
  TValue tv;
  setintptrV(&tv, i);
  return lj_tab_storetv(L, dst, &tv);
}

TValue *lj_tab_storestr(lua_State *L, TValue *dst, GCstr *s)
{
  TValue tv;
  setstrV(L, &tv, s);
  return lj_tab_storetv(L, dst, &tv);
}

TValue *lj_tab_storetab(lua_State *L, TValue *dst, GCtab *tab)
{
  TValue tv;
  settabV(L, &tv, tab);
  return lj_tab_storetv(L, dst, &tv);
}

TValue *lj_tab_storethread(lua_State *L, TValue *dst, lua_State *th)
{
  TValue tv;
  setthreadV(L, &tv, th);
  return lj_tab_storetv(L, dst, &tv);
}

TValue *lj_tab_storeproto(lua_State *L, TValue *dst, GCproto *pt)
{
  TValue tv;
  setprotoV(L, &tv, pt);
  return lj_tab_storetv(L, dst, &tv);
}

TValue *lj_tab_storefunc(lua_State *L, TValue *dst, GCfunc *fn)
{
  TValue tv;
  setfuncV(L, &tv, fn);
  return lj_tab_storetv(L, dst, &tv);
}

TValue *lj_tab_storeudata(lua_State *L, TValue *dst, GCudata *ud)
{
  TValue tv;
  setudataV(L, &tv, ud);
  return lj_tab_storetv(L, dst, &tv);
}

/* -- Table traversal ----------------------------------------------------- */

/* Table traversal indexes:
**
** Array key index: [0 .. t->asize-1]
** Hash key index:  [t->asize .. t->asize+t->hmask]
** Invalid key:     ~0
*/

typedef struct TabKeyIndexSnapshot {
  TValue *array;
  uint32_t asize;
  Node *node;
  MSize hmask;
} TabKeyIndexSnapshot;

static int tab_keyindex_snapshot_current(GCtab *t,
					 const TabKeyIndexSnapshot *snap)
{
  TValue *curarray;
  uint32_t curasize = (uint32_t)lj_tab_array_snapshot_acq(t, &curarray);
  return curasize == snap->asize && curarray == snap->array &&
	 lj_tab_node_acq(t) == snap->node &&
	 !lj_tab_node_is_retiring(snap->node);
}

static LJ_AINLINE int tab_next_array_retry(GCtab *t, TValue *array)
{
  return lj_tab_array_acq(t) != array ||
	 lj_tab_array_is_retiring(t, array) ||
	 lj_tab_array_is_colocated(t, array);
}

static LJ_AINLINE int tab_next_node_retry(GCtab *t, Node *node)
{
  return lj_tab_node_acq(t) != node || lj_tab_node_is_retiring(node);
}

/* Get the successor traversal index of a key with its generation snapshot. */
static uint32_t tab_keyindex_snapshot(GCtab *t, cTValue *origkey,
				      TabKeyIndexSnapshot *snap)
{
  TValue tmp;
  cTValue *key;
retry_all:
  key = origkey;
  snap->asize = (uint32_t)lj_tab_array_snapshot_acq(t, &snap->array);
  snap->node = NULL;
  snap->hmask = 0;
  if (tvisint(key)) {
    int32_t k = intV(key);
    if ((uint32_t)k < snap->asize)
      return (uint32_t)k + 1;
    setnumV(&tmp, (lua_Number)k);
    key = &tmp;
  } else if (tvisnum(key)) {
    int64_t i64;
    int32_t k;
    if (lj_num2int_cond(numV(key), i64, k,
			(uint32_t)i64 < snap->asize))
      return (uint32_t)k + 1;
  }
  if (!tvisnil(key)) {
    Node *n;
    int retry = 1;
  retry_lookup:
    snap->node = lj_tab_node_snapshot_acq(t, &snap->hmask);
    n = hashkey_node(snap->node, snap->hmask, key);
    do {
      TValue nk;
      lj_tv_load_acq(&nk, &n->key);
      if (lj_obj_equal(&nk, key)) {
	if (!tab_keyindex_snapshot_current(t, snap)) {
	  lj_tab_wait_no_l();
	  goto retry_all;
	}
	return snap->asize + (uint32_t)((n+1) - snap->node);
      }
      if (tab_key_read_retry_once(&nk, &retry))
	goto retry_lookup;
    } while ((n = lj_tab_nextnode_acq(n)));
    if (!tab_keyindex_snapshot_current(t, snap)) {
      lj_tab_wait_no_l();
      goto retry_all;
    }
    if (key->u32.hi == LJ_KEYINDEX)  /* Despecialized ITERN while running. */
      return key->u32.lo;
    return ~0u;  /* Invalid key to next. */
  }
  return 0;  /* A nil key starts the traversal. */
}

/* Get the successor traversal index of a key. */
uint32_t LJ_FASTCALL lj_tab_keyindex(GCtab *t, cTValue *key)
{
  TabKeyIndexSnapshot snap;
  return tab_keyindex_snapshot(t, key, &snap);
}

/* Get the next key/value pair of a table traversal. */
int lj_tab_next(GCtab *t, cTValue *key, TValue *o)
{
  uint32_t idx;
  TabKeyIndexSnapshot snap;
retry_next:
  idx = tab_keyindex_snapshot(t, key, &snap);  /* Find successor index of key. */
  tab_test_next_after_keyindex(t, idx);
  if (idx == ~0u && tab_mt_concurrent())
    return 0;
  /* First traverse the array part. */
  for (; idx < snap.asize; idx++) {
    TValue val;
    lj_tv_load_acq(&val, &snap.array[idx]);
    if (tvisforward(&val)) {
      MSize nextasize = snap.asize;
      TValue *nextarray = snap.array;
      if (lj_tab_array_forward_hop_forward(t, &nextarray, &nextasize)) {
	if (idx < nextasize) {
	  snap.array = nextarray;
	  snap.asize = (uint32_t)nextasize;
	  snap.node = NULL;
	  snap.hmask = 0;
	  lj_tv_load_acq(&val, &snap.array[idx]);
	} else {
	  cTValue *tv = lj_tab_getinth(t, (int32_t)idx);
	  snap.node = NULL;
	  snap.hmask = 0;
	  if (tv)
	    lj_tv_load_acq(&val, tv);
	}
      } else if (tab_next_array_retry(t, snap.array)) {
	lj_tab_wait_no_l();
	goto retry_next;
      }
    }
    if (LJ_LIKELY(!tab_val_absent(&val))) {
      if (LJ_UNLIKELY(!tab_tv_snapshot_valid(&val))) {
	if (tab_next_array_retry(t, snap.array)) {
	  lj_tab_wait_no_l();
	  goto retry_next;
	}
	continue;
      }
      setintV(o, idx);
      o[1] = val;
      return 1;
    }
  }
  idx -= snap.asize;
  /* Then traverse the hash part. */
  {
    MSize hmask = snap.node ? snap.hmask : 0;
    Node *node = snap.node ? snap.node : lj_tab_node_snapshot_acq(t, &hmask);
    for (; idx <= hmask; idx++) {
      Node *n = &node[idx];
      TValue key, val;
      lj_tv_load_acq(&val, &n->val);
      if (tvisforward(&val)) {
	lj_tv_load_acq(&key, &n->key);
	if (LJ_UNLIKELY(!tab_tv_snapshot_valid(&key))) {
	  if (tab_next_node_retry(t, node)) {
	    lj_tab_wait_no_l();
	    goto retry_next;
	  }
	  continue;
	}
	if (!tab_hash_key_hidden(&key)) {
	  Node *hopnode = node;
	  MSize hophmask = hmask;
	  if (tab_forwarded_hash_value(t, &hopnode, &hophmask, &key, &val)) {
	    if (LJ_UNLIKELY(!tab_tv_snapshot_valid(&val))) {
	      lj_tab_wait_no_l();
	      goto retry_next;
	    }
	    o[0] = key;
	    o[1] = val;
	    return 1;
	  }
	}
	if (tab_next_node_retry(t, node)) {
	  lj_tab_wait_no_l();
	  goto retry_next;
	}
	continue;
      }
      if (!tab_val_absent(&val)) {
	if (LJ_UNLIKELY(!tab_tv_snapshot_valid(&val))) {
	  if (tab_next_node_retry(t, node)) {
	    lj_tab_wait_no_l();
	    goto retry_next;
	  }
	  continue;
	}
	lj_tv_load_acq(&key, &n->key);
	if (tab_hash_key_hidden(&key)) {
	  /*
	  ** A writer publishes hash insertions as value first, then key.
	  ** next() must not expose KEYLOCK/nil internal keys, but it also
	  ** must not park a reader behind that publication. A second
	  ** acquire snapshot is enough: if the key is still hidden, skip
	  ** this slot for the current traversal; if it was published, the
	  ** value snapshot remains a normal racy Lua table observation.
	  */
	  lj_tv_load_acq(&val, &n->val);
	  lj_tv_load_acq(&key, &n->key);
	  if (tab_hash_key_hidden(&key) || tab_val_absent(&val))
	    continue;
	}
	if (LJ_UNLIKELY(!tab_tv_snapshot_valid(&key) ||
			!tab_tv_snapshot_valid(&val))) {
	  if (tab_next_node_retry(t, node)) {
	    lj_tab_wait_no_l();
	    goto retry_next;
	  }
	  continue;
	}
	o[0] = key;
	o[1] = val;
	return 1;
      }
    }
  }
  return (int32_t)idx < 0 ? -1 : 0;  /* Invalid key or end of traversal. */
}

int32_t LJ_FASTCALL lj_tab_itern_forward(GCtab *t, uint32_t idx, TValue *ctrl)
{
  TValue key;
  int ok;
  key.u32.lo = idx;
  key.u32.hi = LJ_KEYINDEX;
  ok = lj_tab_next(t, &key, ctrl+1);
  if (ok == 1) {
    uint32_t next = lj_tab_keyindex(t, ctrl+1);
    ctrl->u32.lo = next;
    ctrl->u32.hi = LJ_KEYINDEX;
  }
  return ok;
}

int32_t LJ_FASTCALL lj_tab_vmnext_forward(GCtab *t, uint32_t idx, TValue *out)
{
  TValue key, kv[2];
  int ok;
  key.u32.lo = idx;
  key.u32.hi = LJ_KEYINDEX;
  ok = lj_tab_next(t, &key, kv);
  if (ok == 1) {
    out[0] = kv[1];
    out[1] = kv[0];
    return (int32_t)lj_tab_keyindex(t, &kv[0]);
  }
  setnilV(out);
  setnilV(out+1);
  return ok < 0 ? -1 : 0;
}

/* -- Table length calculation -------------------------------------------- */

/* Compute table length. Slow path with mixed array/hash lookups. */
LJ_NOINLINE static MSize tab_len_slow(GCtab *t, size_t hi)
{
  cTValue *tv;
  size_t lo = hi;
  hi++;
  /* Widening search for an upper bound. */
  while ((tv = lj_tab_getint(t, (int32_t)hi)) && !tab_slot_absent_acq(tv)) {
    lo = hi;
    hi += hi;
    if (hi > (size_t)(0x7fffffff - 2)) {  /* Punt and do a linear search. */
      lo = 1;
      while ((tv = lj_tab_getint(t, (int32_t)lo)) && !tab_slot_absent_acq(tv))
	lo++;
      return (MSize)(lo - 1);
    }
  }
  /* Binary search to find a non-nil to nil transition. */
  while (hi - lo > 1) {
    size_t mid = (lo+hi) >> 1;
    cTValue *tvb = lj_tab_getint(t, (int32_t)mid);
    if (tvb && !tab_slot_absent_acq(tvb)) lo = mid; else hi = mid;
  }
  return (MSize)lo;
}

/* Compute table length. Fast path. */
MSize LJ_FASTCALL lj_tab_len(GCtab *t)
{
  TValue *array;
  MSize hmask;
  MSize asize = lj_tab_array_snapshot_acq(t, &array);
  size_t hi = (size_t)asize;
  if (hi) hi--;
  /* In a growing array the last array element is very likely nil. */
  if (hi > 0 &&
      LJ_LIKELY(tab_array_slot_absent_acq(t, &array, &asize, (MSize)hi))) {
    /* Binary search to find a non-nil to nil transition in the array. */
    size_t lo = 0;
    while (hi - lo > 1) {
      size_t mid = (lo+hi) >> 1;
      if (tab_array_slot_absent_acq(t, &array, &asize, (MSize)mid))
	hi = mid;
      else
	lo = mid;
    }
    return (MSize)lo;
  }
  /* Without a hash part, there's an implicit nil after the last element. */
  (void)lj_tab_node_snapshot_acq(t, &hmask);
  return hmask ? tab_len_slow(t, hi) : (MSize)hi;
}

#if LJ_HASJIT
/* Verify hinted table length or compute it. */
MSize LJ_FASTCALL lj_tab_len_hint(GCtab *t, size_t hint)
{
  TValue *array;
  MSize asize = lj_tab_array_snapshot_acq(t, &array);
  if (LJ_LIKELY(hint+1 < asize)) {
    int absent = tab_array_slot_absent_acq(t, &array, &asize, (MSize)hint);
    int nextabsent = tab_array_slot_absent_acq(t, &array, &asize,
					       (MSize)(hint+1));
    if (LJ_LIKELY(!absent && nextabsent))
      return (MSize)hint;
  } else if (hint+1 <= asize) {
    MSize hmask;
    (void)lj_tab_node_snapshot_acq(t, &hmask);
    if (LJ_LIKELY(hmask == 0)) {
      if (!tab_array_slot_absent_acq(t, &array, &asize, (MSize)hint))
	return (MSize)hint;
    }
  }
  return lj_tab_len(t);
}
#endif
