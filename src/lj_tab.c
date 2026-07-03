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
#include "lj_safepoint.h"
#include "lj_tab.h"
#include "lj_tg.h"
#include "lj_thr.h"
#if LJ_HASFFI
#include "lj_cdata.h"
#endif

#include <limits.h>

#define LJ_TAB_MAXCHAIN		8u

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
#if LJ_GC64
  uint64_t old = (uint64_t)(uintptr_t)(*expect);
  int ok = la_cas64(&n->next.ptr64, &old, (uint64_t)(uintptr_t)want,
		    LA_ACQ_REL, LA_ACQ);
  if (!ok)
    *expect = (Node *)(void *)(uintptr_t)old;
  return ok;
#else
  uint32_t old = (uint32_t)(uintptr_t)(*expect);
  int ok = la_cas32(&n->next.ptr32, &old, (uint32_t)(uintptr_t)want,
		    LA_ACQ_REL, LA_ACQ);
  if (!ok)
    *expect = (Node *)(void *)(uintptr_t)old;
  return ok;
#endif
}

static LJ_AINLINE int tab_val_isclaim(cTValue *tv, cTValue *claim)
{
  return tv_rawload(tv) == tv_rawload(claim);
}

static LJ_AINLINE int tab_key_islocked(cTValue *key)
{
  return tviskeylock(key);
}

LJ_FUNCA void lj_tab_wait_no_l(void)
{
  (void)lj_thr_retry_yield(NULL);
}

LJ_FUNCA void lj_tab_wait_l(lua_State *L)
{
  /*
  ** L-aware table retry waits make C/API callers native and safepoint-visible
  ** while preserving the no-state helper for VM/JIT/internal paths where only
  ** TLS ownership is known.
  */
  (void)lj_thr_retry_yield(L);
}

static uint32_t tab_struct_tid(lua_State *L)
{
  TGState *tg = L ? L2TG(L) : lj_thr_get_tg();
  uint32_t tid = tg ? lj_tg_tid_acq(tg) : 0;
  return tid != 0 ? tid : ~(uint32_t)0;
}

#ifdef LJ_TAB_TEST_HELPERS
static uint32_t tab_test_struct_owner_no_l_futex_waits;
static uint32_t tab_test_new0_calls;

static LJ_AINLINE void tab_test_struct_owner_no_l_futex_wait(void)
{
  (void)la_add32_acqrel(&tab_test_struct_owner_no_l_futex_waits, 1);
}

static LJ_AINLINE void tab_test_new0_call(void)
{
  (void)la_add32_acqrel(&tab_test_new0_calls, 1);
}

uint32_t lj_tab_test_struct_owner_no_l_futex_waits(void)
{
  return la_load32_acq(&tab_test_struct_owner_no_l_futex_waits);
}

void lj_tab_test_reset_struct_owner_no_l_futex_waits(void)
{
  la_store32_rel(&tab_test_struct_owner_no_l_futex_waits, 0);
}

uint32_t lj_tab_test_new0_calls(void)
{
  return la_load32_acq(&tab_test_new0_calls);
}

void lj_tab_test_reset_new0_calls(void)
{
  la_store32_rel(&tab_test_new0_calls, 0);
}
#else
#define tab_test_struct_owner_no_l_futex_wait()		((void)0)
#define tab_test_new0_call()				((void)0)
#endif

static void tab_struct_owner_wait(lua_State *L, GCtab *t, uint32_t owner)
{
  if (L) {
    uint32_t actions;
    lj_native_enter(L2TG(L));
    lj_tab_struct_owner_futex_wait(t, owner, 1000000);
    actions = lj_native_leave(L);
    lj_safepoint_checkstop(L, actions);
  } else {
    TGState *tg = lj_thr_get_tg();
    if (tg)
      lj_native_enter(tg);
    tab_test_struct_owner_no_l_futex_wait();
    lj_tab_struct_owner_futex_wait(t, owner, 1000000);
    if (tg)
      (void)lj_tg_in_native_dec_rel(tg);  /* No Lua stack is available to poll. */
  }
}

static LJ_AINLINE int tab_mt_concurrent(void)
{
  TGState *tg = lj_thr_get_tg();
  global_State *g = tg ? tg->gl : NULL;
  return g && (mt_live_acq(g) != 0 || mt_entering_acq(g) != 0);
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
      if (lj_tab_struct_owner_cas(t, &expect, tid))
	return 1;
      owner = expect;
    }
    tab_struct_owner_wait(L, t, owner);
  }
}

void lj_tab_struct_leave(GCtab *t, int acquired)
{
  if (acquired) {
    lj_tab_struct_owner_rel(t, 0);
    lj_tab_struct_owner_futex_wake(t, INT_MAX);
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
    if (root != node || lj_tab_node_is_retiring(node)) {
      lj_tab_wait_no_l();
      return 1;
    }
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
	  if (tvisnil(&nextval) && lj_tab_array_acq(t) == oldarray) {
	    lj_tab_wait_no_l();
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
#if !LJ_GC64
  hdr->reserved = 0;
#endif
  return node;
}

static LJ_AINLINE void tab_node_free(global_State *g, Node *node, MSize hmask)
{
  lj_mem_free(g, lj_tab_node_hdrw(node), lj_tab_node_bytes(hmask));
}

static LJ_AINLINE TValue *tab_array_new(lua_State *L, MSize asize, MSize acap)
{
  TabArrayHdr *hdr = (TabArrayHdr *)lj_mem_new(L, lj_tab_array_bytes(acap));
  lj_tab_array_hdr_init(hdr, asize, acap);
  return lj_tab_array_slots(hdr);
}

static LJ_AINLINE void tab_array_free(global_State *g, TValue *array, MSize acap)
{
  lj_mem_free(g, lj_tab_array_hdrw(array), lj_tab_array_bytes(acap));
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

static LJ_AINLINE void newhpart_publish(GCtab *t, Node *node, MSize hmask,
					Node *freetop)
{
  setfreetop(t, node, freetop);
  lj_tab_node_rel(t, node);
  lj_tab_hmask_rel(t, hmask);
}

/* Create new hash part for table. */
static LJ_AINLINE void newhpart(lua_State *L, GCtab *t, uint32_t hbits)
{
  MSize hmask;
  Node *node = newhpart_alloc(L, hbits, &hmask);
  newhpart_publish(t, node, hmask, &node[hmask+1]);
}

static LJ_AINLINE void tab_storekeyrel(lua_State *L, TValue *dst,
				       cTValue *key)
{
  TValue k;
  copyTV(L, &k, key);
  if (LJ_UNLIKELY(tvismzero(&k)))
    k.u64 = 0;
  copyTVrel(L, dst, &k);
}

static int tab_freeze_forward(TValue *slot, TValue *oldp)
{
  TValue forward;
  setforwardV(&forward);
  for (;;) {
    lj_tv_load_acq(oldp, slot);
    if (tvisforward(oldp))
      return 0;
    if (tvisnil(oldp))
      return 0;
    if (lj_tv_cas(slot, oldp, &forward))
      return 1;  /* M5: old slot ownership moved to its next generation. */
    lj_tab_wait_no_l();
  }
}

static int tab_freeze_forward_any(TValue *slot, TValue *oldp)
{
  TValue forward;
  setforwardV(&forward);
  for (;;) {
    lj_tv_load_acq(oldp, slot);
    if (tvisforward(oldp))
      return 0;
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

static void tab_migrate_store_if_absent(lua_State *L, GCtab *t, TValue *dst,
					cTValue *key, cTValue *val)
{
  UNUSED(t); UNUSED(key);
  (void)tab_store_if_absent_cas(L, dst, val);
}

static TValue *tab_rehash_insert(lua_State *L, Node *nodebase, MSize hmask,
				 Node **freetopp, cTValue *key)
{
  /* Destination belongs to an unpublished replacement hash vector. */
  Node *n = hashkey_node(nodebase, hmask, key);
  if (lj_tab_node_free_reserve(nodebase) != 1)
    lj_assertL(0, "no free node during rehash");
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
			       cTValue *key)
{
  TValue *slot = tab_rehash_arrayslot(array, asize, key);
  return slot ? slot : tab_rehash_insert(L, nodebase, hmask, freetopp, key);
}

static uint32_t tab_rehash_hashcount(Node *oldnode, MSize oldhmask,
				     uint32_t oldasize, uint32_t asize)
{
  uint32_t count = 0;
  if (oldhmask > 0) {
    uint32_t i;
    for (i = 0; i <= oldhmask; i++) {
      Node *n = &oldnode[i];
      TValue key, val;
      uint32_t idx;
    retry_node:
      lj_tv_load_acq(&key, &n->key);
      if (tab_key_islocked(&key)) {
	lj_tab_wait_no_l();
	goto retry_node;
      }
      lj_tv_load_acq(&val, &n->val);
      if (tvisnil(&key)) {
	if (tab_val_is_publish_claim(&val)) {
	  lj_tab_wait_no_l();
	  goto retry_node;
	}
	continue;
      }
      if (tab_hash_key_hidden(&key))
	continue;
      if (!tab_rehash_arrayindex(asize, &key, &idx))
	count++;
    }
  }
  if (asize < oldasize)
    count += oldasize - asize;
  return count;
}

static void tab_retired_push(global_State *g, TabNodeRetire *ret)
{
  TabNodeRetire *head = lj_tab_node_retired_head_acq(g);
  do {
    lj_tab_node_retired_next_rel(ret, head);
  } while (!lj_tab_node_retired_head_cas(g, &head, ret));
  /* 06 section 6.3.5 raw retire. */
}

static TabNodeRetire *tab_retire_reserve(lua_State *L, Node *node,
					 MSize hmask)
{
  TabNodeRetire *ret = lj_mem_newt(L, sizeof(TabNodeRetire), TabNodeRetire);
  lj_tab_node_retired_node_rel(ret, node);
  lj_tab_node_retired_hmask_rel(ret, hmask);
  lj_tab_node_retired_epoch_rel(ret, 0);
  lj_tab_node_retired_armed_rel(ret, 0);
  lj_tab_node_retired_next_rel(ret, NULL);
  return ret;
}

static void tab_retire_discard(global_State *g, TabNodeRetire *ret)
{
  if (ret)
    lj_mem_freet(g, ret);
}

static void tab_retire_arm(global_State *g, TabNodeRetire *ret)
{
  lj_tab_node_retired_epoch_rel(ret, lj_gc2_retire_epoch(g));
  lj_tab_node_retired_armed_rel(ret, 1);
}

static void tab_array_retired_push(global_State *g, TabArrayRetire *ret)
{
  TabArrayRetire *head = lj_tab_array_retired_head_acq(g);
  do {
    lj_tab_array_retired_next_rel(ret, head);
  } while (!lj_tab_array_retired_head_cas(g, &head, ret));
  /* 06 section 6.3.1 raw retire. */
}

static TabArrayRetire *tab_array_retire_reserve(lua_State *L, TValue *array,
						MSize acap)
{
  TabArrayRetire *ret = lj_mem_newt(L, sizeof(TabArrayRetire), TabArrayRetire);
  lj_tab_array_retired_array_rel(ret, array);
  lj_tab_array_retired_acap_rel(ret, acap);
  lj_assertL(!array || acap == lj_tab_array_hdr_acap_acq(array),
	     "mismatched retired table array capacity");
  lj_tab_array_retired_epoch_rel(ret, 0);
  lj_tab_array_retired_armed_rel(ret, 0);
  lj_tab_array_retired_next_rel(ret, NULL);
  return ret;
}

static void tab_array_retire_discard(global_State *g, TabArrayRetire *ret)
{
  if (ret)
    lj_mem_freet(g, ret);
}

static void tab_array_retire_arm(global_State *g, TabArrayRetire *ret)
{
  lj_tab_array_retired_epoch_rel(ret, lj_gc2_retire_epoch(g));
  lj_tab_array_retired_armed_rel(ret, 1);
}

static LJ_AINLINE int tab_retire_epoch_elapsed(uint64_t completed_epoch,
					       uint64_t retire_epoch)
{
  return completed_epoch >= retire_epoch &&
	 completed_epoch - retire_epoch >= LJ_TAB_RETIRE_EPOCHS;
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
#if LJ_GC64
  setmref(t->freetop, nilnode);
#endif
}

static LJ_AINLINE void tab_publish_new(global_State *g, GCtab *t)
{
  newwhite(g, t);
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
static GCtab *newtab(lua_State *L, uint32_t asize, uint32_t hbits)
{
  global_State *g = G(L);
  GCtab *t;
  /* First try to colocate the array part. */
  if (LJ_MAX_COLOSIZE != 0 && asize > 0 && asize <= LJ_MAX_COLOSIZE) {
    TValue *array;
    lj_assertL((sizeof(GCtab) & 7) == 0, "bad GCtab size");
    t = (GCtab *)lj_mem_newgco_unlinked(L, sizetabcolo(asize));
    tab_init_empty(g, t);
    lj_tab_colo_rel(t, (int8_t)asize);
    array = (TValue *)((char *)t + sizeof(GCtab));
    cleararray(array, asize);
    tab_publish_array(t, array, asize, asize);
    tab_publish_new(g, t);
  } else {  /* Otherwise separately allocate the array part. */
    t = (GCtab *)lj_mem_newgco_unlinked(L, sizeof(GCtab));
    tab_init_empty(g, t);
    tab_publish_new(g, t);
    if (asize > 0) {
      TValue *array;
      if (asize > LJ_MAX_ASIZE)
	lj_err_msg(L, LJ_ERR_TABOV);
      array = tab_array_new(L, asize, asize);
      cleararray(array, asize);
      tab_publish_array(t, array, asize, asize);
    }
  }
  if (hbits)
    newhpart(L, t, hbits);
  return t;
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
  return newtab(L, asize, hbits);
}

GCtab * LJ_FASTCALL lj_tab_new0(lua_State *L)
{
  tab_test_new0_call();
  return newtab(L, 0, 0);
}

/* The API of this function conforms to lua_createtable(). */
GCtab *lj_tab_new_ah(lua_State *L, uint32_t a, uint32_t h)
{
  return lj_tab_new(L, a ? a+1 : 0, hsize2hbits(h));
}

#if LJ_HASJIT
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
  int8_t colo = lj_tab_colo_acq(t);
  MSize size = LJ_MAX_COLOSIZE != 0 && colo ?
	       sizetabcolo((uint32_t)colo & 0x7f) : sizeof(GCtab);
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
  uint32_t hashcount;
  uint32_t hash_flags0;
  TabNodeRetire *oldret;
  TabArrayRetire *oldaret;
  int array_next_claimed;
  int struct_acq;
  Node *node_succ;

restart_resize:
  oldnode = lj_tab_node_snapshot_acq(t, &oldhmask);
  oldasize = (uint32_t)lj_tab_array_snapshot_acq(t, &oldarray);
  oldarray_separated = oldarray && !lj_tab_array_is_colocated(t, oldarray);
  oldacap = oldarray_separated ?
    (uint32_t)lj_tab_array_hdr_acap_acq(oldarray) : oldasize;
  array = oldarray;
  newacap = oldacap;
  array_changed = asize != oldasize;
  newarray = 0;
  newhmask = 0;
  newnode = NULL;
  newfreetop = NULL;
  oldret = NULL;
  oldaret = NULL;
  array_next_claimed = 0;
  struct_acq = 0;
  node_succ = NULL;
  hash_flags0 = oldhmask > 0 ? lj_tab_node_hdr_flags_word_acq(oldnode) : 0;
  if (oldhmask > 0 && (hash_flags0 & (uint32_t)TABNODE_FLAG_RETIRING)) {
    lj_tab_wait_no_l();
    goto restart_resize;
  }
  hashcount = tab_rehash_hashcount(oldnode, oldhmask, oldasize, asize);
  if (hashcount) {
    uint32_t needhbits = hsize2hbits(hashcount);
    if (hbits < needhbits)
      hbits = needhbits;
  }
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
  if (newarray) {
    uint32_t i;
    array = tab_array_new(L, asize, newacap);
    for (i = 0; i < newacap; i++)
      lj_tab_storenilraw(&array[i]);
  }
  if (asize > oldasize) {  /* Array part grows? */
    uint32_t i;
    for (i = oldasize; i < asize; i++)  /* Clear newly allocated slots. */
      lj_tab_storenilraw(&array[i]);
  }
  if (hbits) {
    newnode = newhpart_alloc(L, hbits, &newhmask);
    newfreetop = &newnode[newhmask+1];
  }
  if (oldhmask > 0)
    oldret = tab_retire_reserve(L, oldnode, oldhmask);
  if (newarray && oldarray_separated && oldacap > 0)
    oldaret = tab_array_retire_reserve(L, oldarray, oldacap);
  struct_acq = lj_tab_struct_enter(L, t);
  {
    TValue *curarray;
    MSize curasize = lj_tab_array_snapshot_acq(t, &curarray);
    if (curarray != oldarray || (uint32_t)curasize != oldasize ||
	lj_tab_node_acq(t) != oldnode)
      goto retry_resize;
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
    for (i = 0; i < copy; i++) {
      TValue val;
      if (tab_freeze_forward_any(&oldarray[i], &val) &&
	  !tab_val_absent(&val))
	tab_migrate_store_if_absent(L, t, &array[i], NULL, &val);
    }
    tab_test_resize_colocated_after_freeze(L, t, oldarray, oldasize);
  }
  if (newarray && oldarray_separated) {
    uint32_t i;
    for (i = 0; i < oldasize; i++) {
      TValue val;
      if (tab_freeze_forward(&oldarray[i], &val) && !tab_val_absent(&val)) {
	if (i < asize) {
	  tab_migrate_store_if_absent(L, t, &array[i], NULL, &val);
	} else {
	  TValue key;
	  TValue *slot;
	  lj_assertL(hbits != 0, "missing hash part during array tail rehash");
	  setnumV(&key, (lua_Number)i);
	  slot = tab_rehash_insert(L, newnode, newhmask, &newfreetop, &key);
	  tab_migrate_store_if_absent(L, t, slot, &key, &val);
	}
      }
    }
  }
  if (oldhmask > 0) {  /* Reinsert pairs from old hash part. */
    uint32_t i;
    for (i = 0; i <= oldhmask; i++) {
      Node *n = &oldnode[i];
      TValue key, val;
      do {
	lj_tv_load_acq(&key, &n->key);
	if (!tab_key_islocked(&key))
	  break;
	lj_tab_wait_no_l();
      } while (1);
      if (oldret) {
	if (!tab_freeze_forward(&n->val, &val))
	  continue;
      } else {
	lj_tv_load_acq(&val, &n->val);
      }
      if (!tab_val_absent(&val)) {
	TValue *slot;
	if (tab_hash_key_hidden(&key))
	  continue;
	if (hbits) {
	  slot = tab_rehash_slot(L, array, asize, newnode, newhmask,
				 &newfreetop, &key);
	} else {
	  slot = tab_rehash_arrayslot(array, asize, &key);
	  lj_assertL(slot != NULL, "missing hash part during rehash");
	}
	tab_migrate_store_if_absent(L, t, slot, &key, &val);
      }
    }
  }
  if (!oldarray_separated && oldarray && asize < oldasize) {
    /* Freeze and reinsert old colocated array tail off-table. */
    uint32_t i;
    lj_assertL(hbits != 0, "missing hash part during colocated array tail rehash");
    for (i = asize; i < oldasize; i++) {
      TValue key, val;
      if (tab_freeze_forward_any(&oldarray[i], &val) &&
	  !tab_val_absent(&val)) {
	setnumV(&key, (lua_Number)i);
	tab_migrate_store_if_absent(L, t,
	  tab_rehash_insert(L, newnode, newhmask, &newfreetop, &key),
	  &key, &val);
      }
    }
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
    newhpart_publish(t, newnode, newhmask, newfreetop);
    if (oldret)
      tab_retire_arm(G(L), oldret);
  } else {
    global_State *g = G(L);
    lj_tab_hmask_rel(t, 0);
#if LJ_GC64
    setmref(t->freetop, &g->nilnode);
#endif
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
  lj_tab_wait_no_l();
  goto restart_resize;
}

uint32_t lj_tab_reclaim_retired(global_State *g, uint64_t completed_epoch)
{
  TabNodeRetire *ret;
  TabArrayRetire *aret;
  uint32_t reclaimed = 0;
  if (!g || completed_epoch == 0)
    return 0;
  /*
  ** Table readers can carry node/array snapshots through C-side scans. While
  ** more than one TG is live, epoch completion only proves that safepoints
  ** moved forward; it does not prove that no peer still holds such a snapshot.
  ** Defer physical free until the VM is back to a single live TG.
  */
  if (gc2_n_threads_acq(g) > 1)
    return 0;
  ret = lj_tab_node_retired_head_xchg_acqrel(g, NULL);
  while (ret) {
    TabNodeRetire *next = lj_tab_node_retired_next_acq(ret);
    lj_tab_node_retired_next_rel(ret, NULL);
    if (!lj_tab_node_retired_armed_acq(ret)) {
      tab_retired_push(g, ret);
    } else if (tab_retire_epoch_elapsed(completed_epoch,
					lj_tab_node_retired_epoch_acq(ret))) {
      tab_node_free(g, lj_tab_node_retired_node_acq(ret),
		    lj_tab_node_retired_hmask_acq(ret));
      lj_mem_freet(g, ret);
      reclaimed++;
    } else {
      tab_retired_push(g, ret);
    }
    ret = next;
  }
  aret = lj_tab_array_retired_head_xchg_acqrel(g, NULL);
  while (aret) {
    TabArrayRetire *next = lj_tab_array_retired_next_acq(aret);
    lj_tab_array_retired_next_rel(aret, NULL);
    if (!lj_tab_array_retired_armed_acq(aret)) {
      tab_array_retired_push(g, aret);
    } else if (tab_retire_epoch_elapsed(completed_epoch,
					lj_tab_array_retired_epoch_acq(aret))) {
      tab_array_free(g, lj_tab_array_retired_array_acq(aret),
		     lj_tab_array_retired_acap_acq(aret));
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
  while (ret) {
    TabNodeRetire *next = lj_tab_node_retired_next_acq(ret);
    if (lj_tab_node_retired_armed_acq(ret))
      tab_node_free(g, lj_tab_node_retired_node_acq(ret),
		    lj_tab_node_retired_hmask_acq(ret));
    lj_mem_freet(g, ret);
    ret = next;
  }
  aret = lj_tab_array_retired_head_xchg_acqrel(g, NULL);
  while (aret) {
    TabArrayRetire *next = lj_tab_array_retired_next_acq(aret);
    if (lj_tab_array_retired_armed_acq(aret))
      tab_array_free(g, lj_tab_array_retired_array_acq(aret),
		     lj_tab_array_retired_acap_acq(aret));
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
      if (!tab_val_absent(&val))
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
    if (!tab_val_absent(&val)) {
      lj_tv_load_acq(&key, &n->key);
      if (!tab_hash_key_hidden(&key)) {
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

static TValue *tab_rehash_forwarded_key(lua_State *L, GCtab *t, cTValue *key)
{
  rehashtab(L, t, key);
  return lj_tab_set(L, t, key);
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

void lj_tab_reasize(lua_State *L, GCtab *t, uint32_t nasize)
{
  MSize hmask;
  (void)lj_tab_node_snapshot_acq(t, &hmask);
  lj_tab_resize(L, t, nasize+1, hmask > 0 ? lj_fls(hmask)+1 : 0);
}

/* -- Table getters ------------------------------------------------------- */

cTValue * LJ_FASTCALL lj_tab_getinth(GCtab *t, int32_t key)
{
  TValue k;
  Node *node;
  MSize hmask;
  Node *n;
  int key_retry = 1;
  k.n = (lua_Number)key;
retry_lookup:
  node = lj_tab_node_snapshot_acq(t, &hmask);
genlookup:
  if (hmask == 0)
    return NULL;
  n = hashnum_node(node, hmask, &k);
  do {
    TValue nk;
    lj_tv_load_acq(&nk, &n->key);
    if (tvisnum(&nk) && nk.n == k.n) {
      TValue val;
      lj_tv_load_acq(&val, &n->val);
      if (tvisforward(&val) && tab_node_forward_hop(t, &node, &hmask)) {
	cTValue *tv = tab_forwarded_int_arrayslot(t, key);
	if (tv)
	  return tv;
	goto genlookup;
      }
      if (tab_val_forward_retry(t, &val, node))
	goto retry_lookup;
      if (tvisforward(&val))
	return NULL;
      return &n->val;
    }
    if (tab_key_read_retry_once(&nk, &key_retry))
      goto retry_lookup;
  } while ((n = lj_tab_nextnode_acq(n)));
  return NULL;
}

cTValue * LJ_FASTCALL lj_tab_getint_hop(GCtab *t, int32_t key)
{
  return lj_tab_getint(t, key);
}

cTValue *lj_tab_getstr(GCtab *t, const GCstr *key)
{
  Node *node;
  MSize hmask;
  Node *n;
  int key_retry = 1;
retry_lookup:
  node = lj_tab_node_snapshot_acq(t, &hmask);
genlookup:
  if (hmask == 0)
    return NULL;
  n = hashstr_node(node, hmask, key);
  do {
    TValue nk;
    lj_tv_load_acq(&nk, &n->key);
    if (tvisstr(&nk) && strV(&nk) == key) {
      TValue val;
      lj_tv_load_acq(&val, &n->val);
      if (tvisforward(&val) && tab_node_forward_hop(t, &node, &hmask))
	goto genlookup;
      if (tab_val_forward_retry(t, &val, node))
	goto retry_lookup;
      if (tvisforward(&val))
	return NULL;
      return &n->val;
    }
    if (tab_key_read_retry_once(&nk, &key_retry))
      goto retry_lookup;
  } while ((n = lj_tab_nextnode_acq(n)));
  return NULL;
}

cTValue *lj_tab_get(lua_State *L, GCtab *t, cTValue *key)
{
  int key_retry = 1;
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
  retry_lookup:
    node = lj_tab_node_snapshot_acq(t, &hmask);
  genlookup:
    if (hmask == 0)
      return niltv(L);
    n = hashkey_node(node, hmask, key);
    do {
      TValue nk;
      lj_tv_load_acq(&nk, &n->key);
      if (lj_obj_equal(&nk, key)) {
	TValue val;
	lj_tv_load_acq(&val, &n->val);
	if (tvisforward(&val) && tab_node_forward_hop(t, &node, &hmask))
	  goto genlookup;
	if (tab_val_forward_retry(t, &val, node))
	  goto retry_lookup;
	if (tvisforward(&val))
	  return niltv(L);
	return &n->val;
      }
      if (tab_key_read_retry_once(&nk, &key_retry))
	goto retry_lookup;
    } while ((n = lj_tab_nextnode_acq(n)));
  }
  return niltv(L);
}

LJ_FUNCA TValue *lj_tab_gettv_forjit(lua_State *L, GCtab *t, cTValue *key,
				     TValue *out)
{
  for (;;) {
    cTValue *src = lj_tab_get(L, t, key);
    lj_tv_load_acq(out, src);
    if (!tvisforward(out))
      return out;
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

#ifdef LJ_TAB_TEST_HELPERS
static LJTabNewkeyReserveHook tab_test_newkey_anchor_after_reserve_hook;
static LJTabNewkeyReserveHook tab_test_newkey_chain_after_reserve_hook;
static LJTabNextAfterKeyindexHook tab_test_next_after_keyindex_hook;

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

#else
#define tab_test_newkey_anchor_after_reserve(L, t, nodebase) \
  ((void)(L), (void)(t), (void)(nodebase))
#define tab_test_newkey_chain_after_reserve(L, t, nodebase) \
  ((void)(L), (void)(t), (void)(nodebase))
#define tab_test_next_after_keyindex(t, idx) \
  ((void)(t), (void)(idx))
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
TValue *lj_tab_newkey(lua_State *L, GCtab *t, cTValue *key)
{
  Node *nodebase;
  MSize hmask;
  Node *n;
retry_insert:
  nodebase = lj_tab_node_snapshot_acq(t, &hmask);
  if (hmask == 0) {
    rehashtab(L, t, key);
    return lj_tab_set(L, t, key);
  }
  n = hashkey_node(nodebase, hmask, key);
  {
    int locked;
    MSize chainlen;
    TValue *slot = tab_findkey_or_keylock(n, key, &locked, &chainlen);
    if (slot)
      return slot;
    if (locked) {
      lj_tab_wait_no_l();
      goto retry_insert;
    }
    if (chainlen >= LJ_TAB_MAXCHAIN) {
      tab_rehash_chain_overflow(L, t, key, hmask);
      return lj_tab_set(L, t, key);
    }
  }
  {
    TValue nk, nv;
    lj_tv_load_acq(&nk, &n->key);
    if (tab_key_islocked(&nk)) {
      lj_tab_wait_no_l();
      goto retry_insert;
    }
    lj_tv_load_acq(&nv, &n->val);
    if (tvisnil(&nk) && tvisnil(&nv)) {
      int reserved = lj_tab_node_free_reserve(nodebase);
      if (reserved < 0) {
	lj_tab_wait_no_l();
	goto retry_insert;
      }
      if (reserved == 0) {
	rehashtab(L, t, key);
	return lj_tab_set(L, t, key);
      }
      {
	int claimed = tab_try_claim_nil_key(&n->key);
	if (claimed < 0) {
	  lj_tab_node_free_release(nodebase);
	  lj_tab_wait_no_l();
	  goto retry_insert;
	}
	if (claimed == 1) {
	  if (!tab_hash_generation_current(t, nodebase)) {
	    tab_release_claimed_anchor(nodebase, n);
	    lj_tab_wait_no_l();
	    goto retry_insert;
	  }
	  tab_storekeyrel(L, &n->key, key);
	  lj_gc2_barrier_weak_key(L, t, key);
	  lj_gc_pubtab(L, t);
	  lj_assertL(lj_tv_isnil_acq(&n->val), "new hash slot is not empty");
	  if (!tab_hash_generation_current(t, nodebase)) {
	    lj_tab_wait_no_l();
	    return lj_tab_set(L, t, key);
	  }
	  return &n->val;
	}
	lj_tab_node_free_release(nodebase);
      }
      goto retry_insert;
    }
  }
  {
    int locked;
    Node *freenode = tab_claim_free_node_scan(nodebase, hmask, n, &locked);
    lj_assertL(nodebase != &G(L)->nilnode, "insert into fallback hash");
    if (!freenode) {
      if (locked) {
	lj_tab_wait_no_l();
	goto retry_insert;
      }
      rehashtab(L, t, key);
      return lj_tab_set(L, t, key);
    }
    {
      MSize chainlen;
      TValue *slot = tab_findkey_or_keylock(n, key, &locked, &chainlen);
      if (slot) {
	tab_release_claimed_free(nodebase, freenode);
	return slot;
      }
      if (locked) {
	tab_release_claimed_free(nodebase, freenode);
	lj_tab_wait_no_l();
	goto retry_insert;
      }
      if (chainlen >= LJ_TAB_MAXCHAIN) {
	tab_release_claimed_free(nodebase, freenode);
	tab_rehash_chain_overflow(L, t, key, hmask);
	return lj_tab_set(L, t, key);
      }
    }
    lj_assertL(freenode != &G(L)->nilnode, "store to fallback hash");
    {
      Node *next;
      if (!tab_hash_generation_current(t, nodebase)) {
	tab_release_claimed_free(nodebase, freenode);
	lj_tab_wait_no_l();
	goto retry_insert;
      }
      next = lj_tab_nextnode_acq(n);
      lj_tab_nextnode_set(freenode, next);
      if (!tab_nextnode_cas(n, &next, freenode)) {
	tab_release_claimed_free(nodebase, freenode);
	lj_tab_wait_no_l();
	goto retry_insert;
      }
    }
    tab_storekeyrel(L, &freenode->key, key);
    lj_gc2_barrier_weak_key(L, t, key);
    lj_gc_pubtab(L, t);
    lj_assertL(lj_tv_isnil_acq(&freenode->val),
	       "new hash slot is not empty");
    if (!tab_hash_generation_current(t, nodebase)) {
      lj_tab_wait_no_l();
      return lj_tab_set(L, t, key);
    }
    return &freenode->val;
  }
}

int lj_tab_try_newkey_anchor(lua_State *L, GCtab *t, cTValue *key,
			     cTValue *claim, TValue **slot)
{
  MSize hmask;
  Node *nodebase = lj_tab_node_snapshot_acq(t, &hmask);
  Node *n;
  if (hmask == 0)
    return 0;
  n = hashkey_node(nodebase, hmask, key);
  for (;;) {
    TValue nk, nv, expect;
    lj_tv_load_acq(&nk, &n->key);
    if (lj_obj_equal(&nk, key))
      return -1;  /* A racing inserter published the key; retry lookup. */
    if (tviskeylock(&nk)) {
      lj_tab_wait_no_l();  /* Claimed anchor is publishing key. */
      continue;
    }
    if (!tvisnil(&nk))
      return 0;  /* Caller handles collision-chain or resize fallback. */
    lj_tv_load_acq(&nv, &n->val);
    if (!tvisnil(&nv)) {
      lj_tab_wait_no_l();  /* Claimed anchor value is publishing. */
      continue;
    }
    {
      int reserved = lj_tab_node_free_reserve(nodebase);
      if (reserved <= 0)
	return 0;
    }
    tab_test_newkey_anchor_after_reserve(L, t, nodebase);
    if (!tab_hash_generation_current(t, nodebase)) {
      lj_tab_node_free_release(nodebase);
      return -1;
    }
    {
      int claimed = tab_try_claim_nil_key(&n->key);
      if (claimed < 0) {
	lj_tab_node_free_release(nodebase);
	lj_tab_wait_no_l();
	continue;
      }
      if (claimed == 0) {
	lj_tab_node_free_release(nodebase);
	continue;
      }
    }
    if (!tab_hash_generation_current(t, nodebase)) {
      tab_release_claimed_anchor(nodebase, n);
      return -1;
    }
    setnilV(&expect);
    if (lj_tv_cas(&n->val, &expect, claim)) {
      if (!tab_hash_generation_current(t, nodebase)) {
	tab_release_claimed_anchor_value(nodebase, n);
	return -1;
      }
      tab_storekeyrel(L, &n->key, key);
      *slot = lj_tab_set(L, t, key);
      return 1;
    }
    tab_release_claimed_anchor(nodebase, n);
  }
}

int lj_tab_try_newkey_chain(lua_State *L, GCtab *t, cTValue *key,
			    cTValue *claim, TValue **slot)
{
  MSize hmask;
  Node *nodebase = lj_tab_node_snapshot_acq(t, &hmask);
  Node *anchor, *reserved = NULL;
  if (hmask == 0)
    return 0;
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
	return -1;  /* Existing or racing insert for this key; retry lookup. */
      }
      if (tviskeylock(&nk) || (tvisnil(&nk) && tab_val_isclaim(&nv, claim))) {
	lj_tab_wait_no_l();  /* Linked insert is publishing key. */
	goto retry;
      }
    }
    if (!reserved) {
      int reserve = lj_tab_node_free_reserve(nodebase);
      if (reserve <= 0)
	return 0;
      tab_test_newkey_chain_after_reserve(L, t, nodebase);
      if (!tab_hash_generation_current(t, nodebase)) {
	lj_tab_node_free_release(nodebase);
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
	  lj_tab_wait_no_l();  /* Free-node key is publishing. */
	  goto release_retry;
	}
	if (!tvisnil(&nk))
	  continue;
	lj_tv_load_acq(&nv, &n->val);
	if (!tvisnil(&nv)) {
	  if (tab_val_isclaim(&nv, claim)) {
	    lj_tab_wait_no_l();  /* Free-node value is publishing. */
	    goto release_retry;
	  }
	  continue;
	}
	{
	  int claimed = tab_try_claim_nil_key(&n->key);
	  if (claimed < 0) {
	    lj_tab_wait_no_l();  /* Free-node key is publishing. */
	    goto release_retry;
	  }
	  if (claimed == 0)
	    continue;
	}
	if (!tab_hash_generation_current(t, nodebase)) {
	  tab_release_claimed_free(nodebase, n);
	  return -1;
	}
	setnilV(&expect);
	if (lj_tv_cas(&n->val, &expect, claim)) {
	  if (!tab_hash_generation_current(t, nodebase)) {
	    tab_release_claimed_free(nodebase, n);
	    return -1;
	  }
	  reserved = n;  /* Claimed free node; not visible until CAS-prepend. */
	  break;
	}
	lj_tab_storenilraw(&n->key);
      }
      if (!reserved) {
	lj_tab_node_free_release(nodebase);
	return 0;  /* No free node in this hash generation: resize fallback. */
      }
      continue;  /* Re-scan chain before publishing the claimed node. */
    }
    if (LJ_UNLIKELY(anchor == NULL)) {
      tab_release_claimed_free(nodebase, reserved);
      return 0;
    }
    if (!tab_hash_generation_current(t, nodebase)) {
      tab_release_claimed_free(nodebase, reserved);
      return -1;
    }
    n = lj_tab_nextnode_acq(anchor);
    lj_tab_nextnode_set(reserved, n);
    if (tab_nextnode_cas(anchor, &n, reserved)) {
      tab_storekeyrel(L, &reserved->key, key);
      *slot = lj_tab_set(L, t, key);
      return 1;  /* 11.4 FINREG collision insert CAS-prepend. */
    }
  retry:
    continue;
  release_retry:
    lj_tab_node_free_release(nodebase);
    continue;
  found_existing:
    lj_tab_node_free_release(nodebase);
    return -1;
  }
}

TValue *lj_tab_setinth(lua_State *L, GCtab *t, int32_t key)
{
  TValue k;
  Node *node;
  MSize hmask;
  Node *n;
  int key_retry = 1;
  k.n = (lua_Number)key;
retry_lookup:
  node = lj_tab_node_snapshot_acq(t, &hmask);
  if (hmask == 0)
    return lj_tab_newkey(L, t, &k);
  n = hashnum_node(node, hmask, &k);
  do {
    TValue nk;
    lj_tv_load_acq(&nk, &n->key);
    if (tvisnum(&nk) && nk.n == k.n) {
      TValue val;
      lj_tv_load_acq(&val, &n->val);
      if (tvisforward(&val)) {
	TValue *slot = tab_forwarded_setslot(t, &node, &hmask, &k);
	if (slot)
	  return slot;
	if (tab_val_forward_retry(t, &val, node))
	  goto retry_lookup;
	return tab_rehash_forwarded_key(L, t, &k);
      }
      return &n->val;
    }
    if (tab_key_retry_once(&nk, &key_retry))
      goto retry_lookup;
  } while ((n = lj_tab_nextnode_acq(n)));
  return lj_tab_newkey(L, t, &k);
}

TValue *lj_tab_setstr(lua_State *L, GCtab *t, const GCstr *key)
{
  TValue k;
  Node *node;
  MSize hmask;
  Node *n;
  int key_retry = 1;
  setstrV(L, &k, key);
retry_lookup:
  node = lj_tab_node_snapshot_acq(t, &hmask);
  if (hmask == 0)
    return lj_tab_newkey(L, t, &k);
  n = hashstr_node(node, hmask, key);
  do {
    TValue nk;
    lj_tv_load_acq(&nk, &n->key);
    if (tvisstr(&nk) && strV(&nk) == key) {
      TValue val;
      lj_tv_load_acq(&val, &n->val);
      if (tvisforward(&val)) {
	TValue *slot = tab_forwarded_setslot(t, &node, &hmask, &k);
	if (slot)
	  return slot;
	if (tab_val_forward_retry(t, &val, node))
	  goto retry_lookup;
	return tab_rehash_forwarded_key(L, t, &k);
      }
      return &n->val;
    }
    if (tab_key_retry_once(&nk, &key_retry))
      goto retry_lookup;
  } while ((n = lj_tab_nextnode_acq(n)));
  return lj_tab_newkey(L, t, &k);
}

TValue *lj_tab_set(lua_State *L, GCtab *t, cTValue *key)
{
  Node *n;
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
  {
    Node *node;
    MSize hmask;
    int key_retry = 1;
  retry_lookup:
    node = lj_tab_node_snapshot_acq(t, &hmask);
    if (hmask != 0) {
      n = hashkey_node(node, hmask, key);
      do {
	TValue nk;
	lj_tv_load_acq(&nk, &n->key);
	if (lj_obj_equal(&nk, key)) {
	  TValue val;
	  lj_tv_load_acq(&val, &n->val);
	  if (tvisforward(&val)) {
	    TValue *slot = tab_forwarded_setslot(t, &node, &hmask, key);
	    if (slot)
	      return slot;
	    if (tab_val_forward_retry(t, &val, node))
	      goto retry_lookup;
	    return tab_rehash_forwarded_key(L, t, key);
	  }
	  return &n->val;
	}
	if (tab_key_retry_once(&nk, &key_retry))
	  goto retry_lookup;
      } while ((n = lj_tab_nextnode_acq(n)));
    }
  }
  return lj_tab_newkey(L, t, key);
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

LJ_FUNCA int lj_tab_trystoretv_cas(lua_State *L, TValue *dst, cTValue *src)
{
  TValue old;
  UNUSED(L);
  for (;;) {
    lj_tv_load_acq(&old, dst);
    if (tvisforward(&old))
      return LJ_TAB_STORE_CAS_FORWARD;
    if (lj_tv_cas(dst, &old, src))
      return LJ_TAB_STORE_CAS_OK;  /* 06 section 6.3.2: CAS-published store. */
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

LJ_FUNCA int lj_tab_read_current_keyed(GCtab *parent, TValue *dst,
				       cTValue *key, TValue *oldp)
{
  if (!tab_current_slot_for_key(parent, dst, key))
    return LJ_TAB_STORE_CAS_STALE;
  lj_tv_load_acq(oldp, dst);
  if (tvisforward(oldp))
    return LJ_TAB_STORE_CAS_FORWARD;
  if (!tab_current_slot_for_key(parent, dst, key))
    return LJ_TAB_STORE_CAS_STALE;
  return LJ_TAB_STORE_CAS_OK;
}

LJ_FUNCA int lj_tab_trystoretv_cas_keyed(lua_State *L, GCtab *parent,
					 TValue *dst, cTValue *key,
					 cTValue *src)
{
  int rc;
  if (!parent)
    return lj_tab_trystoretv_cas(L, dst, src);
  if (!tab_current_slot_for_key(parent, dst, key))
    return LJ_TAB_STORE_CAS_STALE;
  rc = lj_tab_trystoretv_cas(L, dst, src);
  if (rc != LJ_TAB_STORE_CAS_OK)
    return rc;
  if (tab_current_slot_for_key(parent, dst, key))
    return LJ_TAB_STORE_CAS_OK;
  lj_gc_pubtabtv(L, parent, dst);
  return LJ_TAB_STORE_CAS_STALE;
}

LJ_FUNCA int lj_tab_trysetnil_cas_keyed(lua_State *L, GCtab *parent,
					TValue *dst, cTValue *key,
					cTValue *src, TValue *oldp)
{
  TValue expect;
  int rc;
  for (;;) {
    rc = lj_tab_read_current_keyed(parent, dst, key, oldp);
    if (rc != LJ_TAB_STORE_CAS_OK)
      return rc;
    if (!tvisnil(oldp))
      return LJ_TAB_STORE_CAS_EXISTS;
    expect = *oldp;
    if (lj_tv_cas(dst, &expect, src)) {
      if (tab_current_slot_for_key(parent, dst, key))
	return LJ_TAB_STORE_CAS_OK;
      lj_gc_pubtabtv(L, parent, dst);
      return LJ_TAB_STORE_CAS_STALE;
    }
    *oldp = expect;
    if (tvisforward(&expect))
      return LJ_TAB_STORE_CAS_FORWARD;
    if (!tvisnil(&expect))
      return LJ_TAB_STORE_CAS_EXISTS;
    lj_tab_store_wait_l(L);
  }
}

static int tab_clear_try_nil_keyed(lua_State *L, GCtab *parent, TValue *dst,
				   cTValue *key)
{
  TValue old, expect, nilv;
  setnilV(&nilv);
  for (;;) {
    int rc = lj_tab_read_current_keyed(parent, dst, key, &old);
    if (rc != LJ_TAB_STORE_CAS_OK)
      return 0;
    if (tvisnil(&old))
      return 1;
    if (tab_val_is_publish_claim(&old)) {
      lj_tab_wait_l(L);
      continue;
    }
    expect = old;
    if (lj_tv_cas(dst, &expect, &nilv))
      return tab_current_slot_for_key(parent, dst, key);
    if (tvisforward(&expect))
      return 0;
    if (tab_val_is_publish_claim(&expect)) {
      lj_tab_wait_l(L);
      continue;
    }
    lj_tab_store_wait_l(L);
  }
}

static void tab_clear_array_shared(lua_State *L, GCtab *t, TValue *array,
				   MSize asize)
{
  MSize i;
  for (i = 0; i < asize; i++) {
    TValue key;
    if (i > (MSize)INT32_MAX)
      break;
    setintV(&key, (int32_t)i);
    while (!tab_clear_try_nil_keyed(L, t, &array[i], &key))
      lj_tab_store_wait_l(L);
  }
}

static void tab_clear_hash_slot_shared(lua_State *L, GCtab *t, Node *n)
{
  for (;;) {
    TValue key, val;
    lj_tv_load_acq(&val, &n->val);
    if (tab_val_is_publish_claim(&val)) {
      lj_tab_wait_l(L);
      continue;
    }
    if (tab_val_absent(&val))
      return;
    lj_tv_load_acq(&key, &n->key);
    if (tab_key_islocked(&key)) {
      lj_tab_wait_l(L);
      continue;
    }
    if (tab_hash_key_hidden(&key))
      return;
    (void)tab_clear_try_nil_keyed(L, t, &n->val, &key);
    return;
  }
}

static void tab_clear_shared(lua_State *L, GCtab *t)
{
  TValue *array;
  Node *node;
  MSize asize, hmask, i;
  int guard = lj_tab_struct_enter(L, t);
  asize = lj_tab_array_snapshot_acq(t, &array);
  if (array)
    tab_clear_array_shared(L, t, array, asize);
  node = lj_tab_node_snapshot_acq(t, &hmask);
  if (hmask > 0) {
    for (i = 0; i <= hmask; i++)
      tab_clear_hash_slot_shared(L, t, &node[i]);
  }
  lj_tab_struct_leave(t, guard);
}

/* Clear a table. */
void LJ_FASTCALL lj_tab_clear(lua_State *L, GCtab *t)
{
  if (mt_active_acq(G(L)))
    tab_clear_shared(L, t);
  else
    tab_clear_raw(t);
}

LJ_FUNCA int32_t lj_tab_storetv_existing_forjit(lua_State *L, GCtab *parent,
						cTValue *key, cTValue *src)
{
  for (;;) {
    TValue *dst = (TValue *)lj_tab_get(L, parent, key);
    TValue old, expect;
    int weakwr;
    lj_tv_load_acq(&old, dst);
    if (tvisforward(&old)) {
      lj_tab_store_wait_l(L);
      continue;
    }
    if (tvisnil(&old))
      return 0;  /* Let the interpreter resolve __newindex/new-key semantics. */
    if (!tab_current_slot_for_key(parent, dst, key)) {
      lj_tab_store_wait_l(L);
      continue;
    }
    weakwr = lj_gc2_weak_write_begin(L, parent);
    if (weakwr) {
      lj_gc2_barrier_weak_write(L, parent, key, src);
      lj_gc2_barrier_tv_pair(L, obj2gco(parent), src);
    }
    expect = old;
    if (lj_tv_cas(dst, &expect, src)) {
      if (tab_current_slot_for_key(parent, dst, key)) {
	if (weakwr) {
	  lj_gc2_barrier_weak_write(L, parent, key, src);
	  lj_gc2_barrier_tv_pair(L, obj2gco(parent), src);
	  lj_gc2_weak_write_end(L, weakwr);
	} else {
	  lj_gc2_barrier_weak_write(L, parent, key, dst);
	  lj_gc2_barrier_tv_pair(L, obj2gco(parent), dst);
	}
	return 1;
      }
      lj_gc_pubtabtv(L, parent, dst);
      if (weakwr)
	lj_gc2_weak_write_end(L, weakwr);
      lj_tab_store_wait_l(L);
      continue;
    }
    if (weakwr)
      lj_gc2_weak_write_end(L, weakwr);
    if (tvisforward(&expect)) {
      lj_tab_store_wait_l(L);
      continue;
    }
    if (tvisnil(&expect))
      return 0;
    lj_tab_store_wait_l(L);
  }
}

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
    if (tvisforward(&val) ?
	lj_tab_array_forward_hop_forward(parent, &array, &asize) :
	tab_array_forward_hop_writer(parent, &array, &asize)) {
      if (idx < asize)
	return &array[idx];
      return lj_tab_setinth(L, parent, (int32_t)key);
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
					   TValue *keycopy, cTValue **keyp)
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
  if (tab_key_islocked(keycopy) || tvisnil(keycopy))
    return dst;
  *keyp = keycopy;
  {
    TValue *slot = tab_forwarded_setslot(parent, &node, &hmask, keycopy);
    return slot ? slot : lj_tab_set(L, parent, keycopy);
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
					     &keycopy, &barrier_key);
  if (slot == dst)
    return slot;
  return lj_tab_trystoretv_cas_keyed(L, parent, slot, barrier_key, src) ==
	 LJ_TAB_STORE_CAS_OK ? slot : NULL;
}
#endif

static TValue *tab_current_jit_array_slot(lua_State *L, GCtab *parent,
					  TValue *orig, MSize key)
{
  for (;;) {
    TValue *array;
    MSize asize, idx;
    asize = tab_store_array_snapshot_acq(parent, &array);
    if (tab_ptr_index((uintptr_t)array, (uintptr_t)orig, sizeof(TValue),
		      asize, &idx)) {
      if (lj_tab_array_is_retiring(parent, array)) {
	TValue *next = array;
	MSize nextasize = asize;
	if (tab_array_forward_hop_writer(parent, &next, &nextasize)) {
	  if (idx < nextasize)
	    return &next[idx];
	  return lj_tab_setinth(L, parent, (int32_t)key);
	}
	lj_tab_store_wait_l(L);
	continue;
      }
      return tab_forwarded_jit_array_slot(L, parent, array, asize, orig, key);
    }
    return lj_tab_setint(L, parent, (int32_t)key);
  }
}

static LJ_AINLINE int tab_jit_array_current_match(GCtab *parent,
						  TValue *orig, MSize key)
{
  TValue *array;
  MSize asize;
  uintptr_t base;
  asize = tab_store_array_snapshot_acq(parent, &array);
  base = (uintptr_t)(void *)orig - (uintptr_t)key * sizeof(TValue);
  return key < asize && array == (TValue *)(void *)base &&
	 !lj_tab_array_is_retiring(parent, array);
}

static LJ_AINLINE int tab_jit_hash_current_match(GCtab *parent,
						 TValue *orig)
{
  Node *node;
  MSize hmask = tab_store_node_snapshot_acq(parent, &node);
  MSize idx;
  return tab_ptr_index((uintptr_t)node, (uintptr_t)orig, sizeof(Node),
		       hmask + 1u, &idx) && !lj_tab_node_is_retiring(node);
}

static TValue *tab_current_jit_hash_slot(lua_State *L, GCtab *parent,
					 TValue *orig, cTValue *key,
					 TValue *keycopy, cTValue **keyp)
{
  Node *node, *n;
  MSize hmask, idx;
  hmask = tab_store_node_snapshot_acq(parent, &node);
  if (tab_ptr_index((uintptr_t)node, (uintptr_t)orig, sizeof(Node),
			    hmask + 1u, &idx)) {
    if (lj_tab_node_is_retiring(node)) {
      TValue *slot;
      n = &node[idx];
      lj_tv_load_acq(keycopy, &n->key);
      if (!tab_key_islocked(keycopy) && !tvisnil(keycopy)) {
	*keyp = keycopy;
	slot = tab_forwarded_setslot(parent, &node, &hmask, keycopy);
	if (slot)
	  return slot;
      }
      *keyp = key;
      return lj_tab_set(L, parent, key);
    }
    return tab_forwarded_jit_hash_slot(L, parent, node, hmask, orig,
				       keycopy, keyp);
  }
  *keyp = key;
  return lj_tab_set(L, parent, key);
}

LJ_FUNCA TValue *lj_tab_storetv_forjit_array_nogc(lua_State *L,
						  GCtab *parent,
						  TValue *dst, cTValue *src,
						  MSize key)
{
  TValue *orig = dst;
  TValue keytv;
  setintV(&keytv, (int32_t)key);
  if (tab_jit_array_current_match(parent, orig, key) &&
      lj_tab_trystoretv_cas_keyed(L, parent, orig, &keytv, src) ==
      LJ_TAB_STORE_CAS_OK)
    return orig;
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
  int weakwr = lj_gc2_weak_write_begin(L, parent);
  TValue keytv;
  if (weakwr) {
    setintV(&keytv, (int32_t)key);
    lj_gc2_barrier_weak_write(L, parent, &keytv, src);
    lj_gc2_barrier_tv_pair(L, obj2gco(parent), src);
  }
  dst = lj_tab_storetv_forjit_array_nogc(L, parent, dst, src, key);
  if (weakwr) {
    lj_gc2_barrier_weak_write(L, parent, &keytv, src);
    lj_gc2_barrier_tv_pair(L, obj2gco(parent), src);
    lj_gc2_weak_write_end(L, weakwr);
  } else {
    lj_gc2_barrier_weak_write(L, parent, NULL, dst);  /* M8: traced weak-value array write. */
    lj_gc2_barrier_tv_pair(L, obj2gco(parent), dst);  /* M10: traced parent barrier. */
  }
  return dst;
}

LJ_FUNCA TValue *lj_tab_storetv_forvm_array(lua_State *L, GCtab *parent,
					    TValue *dst, cTValue *src,
					    MSize key)
{
  TValue *orig = dst;
  TValue keytv;
  int weakwr = lj_gc2_weak_write_begin(L, parent);
  /* The x64 VM runs its existing table barrier sequence after this helper. */
  setintV(&keytv, (int32_t)key);
  if (weakwr) {
    lj_gc2_barrier_weak_write(L, parent, &keytv, src);
    lj_gc2_barrier_tv_pair(L, obj2gco(parent), src);
  }
  for (;;) {
    dst = tab_current_jit_array_slot(L, parent, orig, key);
    if (lj_tab_trystoretv_cas_keyed(L, parent, dst, &keytv, src) ==
	LJ_TAB_STORE_CAS_OK)
      break;
    lj_tab_store_wait_l(L);  /* VM array store saw stale/FORWARD slot. */
  }
  if (weakwr) {
    lj_gc2_barrier_weak_write(L, parent, &keytv, src);
    lj_gc2_barrier_tv_pair(L, obj2gco(parent), src);
    lj_gc2_weak_write_end(L, weakwr);
  }
  return dst;
}

LJ_FUNCA TValue *lj_tab_storetv_forvm_strhash(lua_State *L, GCtab *parent,
					      TValue *dst, cTValue *src,
					      GCstr *key)
{
  TValue keytv, keycopy;
  TValue *orig = dst;
  cTValue *barrier_key;
  int weakwr;
  setstrV(L, &keytv, key);
  barrier_key = &keytv;
  weakwr = lj_gc2_weak_write_begin(L, parent);
  /* The x64 VM runs its existing table barrier sequence after this helper. */
  if (weakwr) {
    lj_gc2_barrier_weak_write(L, parent, &keytv, src);
    lj_gc2_barrier_tv_pair(L, obj2gco(parent), src);
  }
  if (tab_jit_hash_current_match(parent, orig) &&
      lj_tab_trystoretv_cas_keyed(L, parent, orig, &keytv, src) ==
      LJ_TAB_STORE_CAS_OK) {
    dst = orig;
    goto done;
  }
  for (;;) {
    dst = tab_current_jit_hash_slot(L, parent, orig, &keytv, &keycopy,
				    &barrier_key);
    if (lj_tab_trystoretv_cas_keyed(L, parent, dst, barrier_key, src) ==
	LJ_TAB_STORE_CAS_OK)
      break;
    lj_tab_store_wait_l(L);  /* VM hash store saw stale/FORWARD slot. */
  }
done:
  if (weakwr) {
    lj_gc2_barrier_weak_write(L, parent, barrier_key, src);
    lj_gc2_barrier_tv_pair(L, obj2gco(parent), src);
    lj_gc2_weak_write_end(L, weakwr);
  }
  return dst;
}

LJ_FUNCA TValue *lj_tab_storetv_forjit_hash(lua_State *L, GCtab *parent,
					    TValue *dst, cTValue *src,
					    cTValue *key)
{
  TValue keycopy;
  TValue *orig = dst;
  cTValue *barrier_key = key;
  int weakwr = lj_gc2_weak_write_begin(L, parent);
  if (weakwr) {
    lj_gc2_barrier_weak_write(L, parent, key, src);
    lj_gc2_barrier_tv_pair(L, obj2gco(parent), src);
  }
  if (tab_jit_hash_current_match(parent, orig) &&
      lj_tab_trystoretv_cas_keyed(L, parent, orig, key, src) ==
      LJ_TAB_STORE_CAS_OK) {
    dst = orig;
    goto done;
  }
  for (;;) {
    dst = tab_current_jit_hash_slot(L, parent, orig, key, &keycopy,
				    &barrier_key);
    if (lj_tab_trystoretv_cas_keyed(L, parent, dst, barrier_key, src) ==
	LJ_TAB_STORE_CAS_OK)
      break;
    lj_tab_store_wait_l(L);  /* JIT hash store saw stale/FORWARD slot. */
  }
done:
  if (weakwr) {
    lj_gc2_barrier_weak_write(L, parent, barrier_key, src);
    lj_gc2_barrier_tv_pair(L, obj2gco(parent), src);
    lj_gc2_weak_write_end(L, weakwr);
  } else {
    lj_gc2_barrier_weak_write(L, parent, barrier_key, dst);  /* M8: traced weak hash write. */
    lj_gc2_barrier_tv_pair(L, obj2gco(parent), dst);  /* M10: traced parent barrier. */
  }
  return dst;
}

LJ_FUNCA TValue *lj_tab_storetv_forjit_newref(lua_State *L, GCtab *parent,
					      TValue *dst, cTValue *src,
					      cTValue *key)
{
  int weakwr = lj_gc2_weak_write_begin(L, parent);
  if (weakwr) {
    lj_gc2_barrier_weak_write(L, parent, key, src);
    lj_gc2_barrier_tv_pair(L, obj2gco(parent), src);
  }
  for (;;) {
    dst = lj_tab_set(L, parent, key);
    if (lj_tab_trystoretv_cas_keyed(L, parent, dst, key, src) ==
	LJ_TAB_STORE_CAS_OK)
      break;
    lj_tab_store_wait_l(L);  /* JIT NEWREF store saw stale/FORWARD slot. */
  }
  if (weakwr) {
    lj_gc2_barrier_weak_write(L, parent, key, src);
    lj_gc2_barrier_tv_pair(L, obj2gco(parent), src);
    lj_gc2_weak_write_end(L, weakwr);
  } else {
    lj_gc2_barrier_weak_write(L, parent, key, dst);  /* M8: traced NEWREF weak write. */
    lj_gc2_barrier_tv_pair(L, obj2gco(parent), dst);  /* M10: traced parent barrier. */
  }
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
  TValue *array;
  MSize asize;
  asize = tab_store_array_snapshot_acq(parent, &array);
  if (array && !lj_tab_array_is_colocated(parent, array)) {
    if (lj_tab_array_is_retiring(parent, array)) {
      TValue *next = array;
      MSize nextasize = asize;
      TValue val;
      int observed_forward = 0;
      if (key < asize) {
	lj_tv_load_acq(&val, &array[key]);
	observed_forward = tvisforward(&val);
      }
      if (observed_forward ?
	  lj_tab_array_forward_hop_forward(parent, &next, &nextasize) :
	  lj_tab_array_forward_hop(parent, &next, &nextasize)) {
	if (key < nextasize)
	  return &next[key];
	return lj_tab_setinth(L, parent, (int32_t)key);
      }
    }
  }
  if (array && key < asize)
    return tab_current_jit_array_slot(L, parent, &array[key], key);
  return lj_tab_setint(L, parent, (int32_t)key);
}

static void tab_tsetm_barrier_range(lua_State *L, GCtab *parent, uint32_t start,
				    uint32_t n)
{
  global_State *g = G(L);
  uint32_t i;
  for (i = 0; i < n; i++) {
    TValue *dst = tab_current_vm_array_key_slot(L, parent, (MSize)(start + i));
    lj_gc2_barrier_tv_pair_g(g, obj2gco(parent), dst);
  }
  lj_gc2_barrier_tab(L, parent);  /* Preserve the previous TSETM table barrier. */
  if (!isblack(obj2gco(parent)))
    return;
  for (i = 0; i < n; i++) {
    TValue snap;
    TValue *dst = tab_current_vm_array_key_slot(L, parent, (MSize)(start + i));
    lj_tv_load_acq(&snap, dst);
    if (tviswhite(&snap)) {
      lj_gc_barrierback(g, parent);
      return;
    }
  }
}

LJ_FUNCA void lj_tab_storetvn_forvm_array(lua_State *L, GCtab *parent,
					  uint32_t start, cTValue *src,
					  uint32_t n)
{
  uint32_t i;
  int weakwr;
  if (!L || !parent || !src || n == 0)
    return;
  weakwr = lj_gc2_weak_write_begin(L, parent);
  for (i = 0; i < n; i++) {
    TValue *dst;
    TValue key;
    setintV(&key, (int32_t)(start + i));
    if (weakwr) {
      lj_gc2_barrier_weak_write(L, parent, &key, &src[i]);
      lj_gc2_barrier_tv_pair(L, obj2gco(parent), &src[i]);
    }
    for (;;) {
      dst = tab_current_vm_array_key_slot(L, parent, (MSize)(start + i));
      if (lj_tab_trystoretv_cas_keyed(L, parent, dst, &key, &src[i]) ==
	  LJ_TAB_STORE_CAS_OK)
	break;
      lj_tab_store_wait_l(L);  /* VM TSETM saw stale/FORWARD slot. */
    }
  }
  if (weakwr || tab_tsetm_barrier_needed(L, parent))
    tab_tsetm_barrier_range(L, parent, start, n);
  lj_gc2_weak_write_end(L, weakwr);
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
      } else if (lj_tab_array_acq(t) != snap.array ||
		 lj_tab_array_is_retiring(t, snap.array) ||
		 lj_tab_array_is_colocated(t, snap.array)) {
	lj_tab_wait_no_l();
	goto retry_next;
      }
    }
    if (LJ_LIKELY(!tab_val_absent(&val))) {
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
	if (!tab_hash_key_hidden(&key)) {
	  Node *hopnode = node;
	  MSize hophmask = hmask;
	  if (tab_forwarded_hash_value(t, &hopnode, &hophmask, &key, &val)) {
	    o[0] = key;
	    o[1] = val;
	    return 1;
	  }
	}
	if (lj_tab_node_acq(t) != node || lj_tab_node_is_retiring(node)) {
	  lj_tab_wait_no_l();
	  goto retry_next;
	}
	continue;
      }
      if (!tab_val_absent(&val)) {
	lj_tv_load_acq(&key, &n->key);
	if (tab_hash_key_hidden(&key)) {
	  lj_tab_wait_no_l();
	  lj_tv_load_acq(&val, &n->val);
	  lj_tv_load_acq(&key, &n->key);
	  if (tab_hash_key_hidden(&key) || tab_val_absent(&val))
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
