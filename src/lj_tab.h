/*
** Table handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_TAB_H
#define _LJ_TAB_H

#include "lj_obj.h"

/* Hash constants. Tuned using a brute force search. */
#define HASH_BIAS	(-0x04c11db7)
#define HASH_ROT1	14
#define HASH_ROT2	5
#define HASH_ROT3	13

/* Scramble the bits of numbers and pointers. */
static LJ_AINLINE uint32_t hashrot(uint32_t lo, uint32_t hi)
{
#if LJ_TARGET_X86ORX64
  /* Prefer variant that compiles well for a 2-operand CPU. */
  lo ^= hi; hi = lj_rol(hi, HASH_ROT1);
  lo -= hi; hi = lj_rol(hi, HASH_ROT2);
  hi ^= lo; hi -= lj_rol(lo, HASH_ROT3);
#else
  lo ^= hi;
  lo = lo - lj_rol(hi, HASH_ROT1);
  hi = lo ^ lj_rol(hi, HASH_ROT1 + HASH_ROT2);
  hi = hi - lj_rol(lo, HASH_ROT3);
#endif
  return hi;
}

/* Hash values are masked with the table hash mask and used as an index. */
static LJ_AINLINE Node *hashmask_node(Node *n, MSize hmask, uint32_t hash)
{
  return &n[hash & hmask];
}

static LJ_AINLINE Node *hashmask(const GCtab *t, uint32_t hash)
{
  MSize hmask;
  Node *n = lj_tab_node_snapshot_acq(t, &hmask);
  return hashmask_node(n, hmask, hash);
}

/* String IDs are generated when a string is interned. */
#define hashstr_node(n, hmask, s)	hashmask_node((n), (hmask), (s)->sid)
#define hashstr(t, s)		hashmask(t, (s)->sid)

#define hashlohi_node(n, hmask, lo, hi) \
  hashmask_node((n), (hmask), hashrot((lo), (hi)))
#define hashlohi(t, lo, hi)	hashmask((t), hashrot((lo), (hi)))
#define hashnum_node(n, hmask, o) \
  hashlohi_node((n), (hmask), (o)->u32.lo, ((o)->u32.hi << 1))
#define hashnum(t, o)		hashlohi((t), (o)->u32.lo, ((o)->u32.hi << 1))
#define hashgcref_node(n, hmask, r) \
  hashlohi_node((n), (hmask), (uint32_t)gcrefu_acq(r), \
		(uint32_t)(gcrefu_acq(r) >> 32))
#define hashgcref(t, r) \
  hashlohi((t), (uint32_t)gcrefu_acq(r), \
	   (uint32_t)(gcrefu_acq(r) >> 32))

#define hsize2hbits(s)	((s) ? ((s)==1 ? 1 : 1+lj_fls((uint32_t)((s)-1))) : 0)
#define LJ_TAB_RETIRE_EPOCHS	2u

/* Node and array retire records share the same Treiber-list metadata. */
#define LJ_TAB_RETIRE_HEAD_ACCESSORS(name, type, field) \
static LJ_AINLINE type *name##_head_acq(const global_State *g) \
{ \
  return (type *)la_loadptr_acq((void *const *)&g->tab.field); \
} \
\
static LJ_AINLINE int name##_head_cas(global_State *g, type **oldp, type *ret) \
{ \
  return la_casptr((void **)&g->tab.field, (void **)oldp, ret, \
		   LA_ACQ_REL, LA_ACQ); \
} \
\
static LJ_AINLINE type *name##_head_xchg_acqrel(global_State *g, type *ret) \
{ \
  return (type *)la_xchgptr_acqrel((void **)&g->tab.field, ret); \
}

#define LJ_TAB_RETIRE_RECORD_ACCESSORS(name, type) \
static LJ_AINLINE type *name##_next_acq(const type *ret) \
{ \
  return (type *)la_loadptr_acq((void *const *)&ret->next); \
} \
\
static LJ_AINLINE void name##_next_rel(type *ret, type *next) \
{ \
  la_storeptr_rel((void **)&ret->next, next); \
} \
\
static LJ_AINLINE uint64_t name##_epoch_acq(const type *ret) \
{ \
  return la_load64_acq(&ret->retire_epoch); \
} \
\
static LJ_AINLINE void name##_epoch_rel(type *ret, uint64_t epoch) \
{ \
  la_store64_rel(&ret->retire_epoch, epoch); \
} \
\
static LJ_AINLINE uint32_t name##_armed_acq(const type *ret) \
{ \
  return la_load32_acq(&ret->armed); \
} \
\
static LJ_AINLINE void name##_armed_rel(type *ret, uint32_t armed) \
{ \
  la_store32_rel(&ret->armed, armed); \
}

LJ_TAB_RETIRE_HEAD_ACCESSORS(lj_tab_node_retired, TabNodeRetire, retired_nodes)
LJ_TAB_RETIRE_RECORD_ACCESSORS(lj_tab_node_retired, TabNodeRetire)

static LJ_AINLINE GCtab *
lj_tab_node_retired_tab_acq(const TabNodeRetire *ret)
{
  return (GCtab *)la_loadptr_acq((void *const *)&ret->tab);
}

static LJ_AINLINE Node *lj_tab_node_retired_node_acq(const TabNodeRetire *ret)
{
  return (Node *)la_loadptr_acq((void *const *)&ret->node);
}

static LJ_AINLINE void lj_tab_node_retired_node_rel(TabNodeRetire *ret,
						    Node *node)
{
  la_storeptr_rel((void **)&ret->node, node);
}

static LJ_AINLINE MSize lj_tab_node_retired_hmask_acq(const TabNodeRetire *ret)
{
  return (MSize)la_load32_acq(&ret->hmask);
}

static LJ_AINLINE void lj_tab_node_retired_hmask_rel(TabNodeRetire *ret,
						     MSize hmask)
{
  la_store32_rel(&ret->hmask, hmask);
}

LJ_TAB_RETIRE_HEAD_ACCESSORS(lj_tab_array_retired, TabArrayRetire,
			     retired_arrays)
LJ_TAB_RETIRE_RECORD_ACCESSORS(lj_tab_array_retired, TabArrayRetire)

static LJ_AINLINE GCtab *
lj_tab_array_retired_tab_acq(const TabArrayRetire *ret)
{
  return (GCtab *)la_loadptr_acq((void *const *)&ret->tab);
}

static LJ_AINLINE TValue *
lj_tab_array_retired_array_acq(const TabArrayRetire *ret)
{
  return (TValue *)la_loadptr_acq((void *const *)&ret->array);
}

static LJ_AINLINE void lj_tab_array_retired_array_rel(TabArrayRetire *ret,
						      TValue *array)
{
  la_storeptr_rel((void **)&ret->array, array);
}

static LJ_AINLINE MSize lj_tab_array_retired_acap_acq(const TabArrayRetire *ret)
{
  return (MSize)la_load32_acq(&ret->acap);
}

static LJ_AINLINE void lj_tab_array_retired_acap_rel(TabArrayRetire *ret,
						     MSize acap)
{
  la_store32_rel(&ret->acap, acap);
}

#undef LJ_TAB_RETIRE_RECORD_ACCESSORS
#undef LJ_TAB_RETIRE_HEAD_ACCESSORS

/* A construction root closes the gap between a table becoming READY and its
** first ordinary Lua/native semantic root. The rooted API leaves this TG slot
** live so callers may safely wait, reacquire a state claim, or grow a stack. */
typedef struct LJTabRoot {
  TGState *tg;
  uint32_t idx;
} LJTabRoot;

LJ_FUNCA GCtab *lj_tab_new(lua_State *L, uint32_t asize, uint32_t hbits);
LJ_FUNCA GCtab * LJ_FASTCALL lj_tab_new0(lua_State *L);
LJ_FUNC GCtab *lj_tab_new_ah(lua_State *L, uint32_t a, uint32_t h);
LJ_FUNC GCtab *lj_tab_new_rooted(lua_State *L, uint32_t asize,
				  uint32_t hbits, LJTabRoot *root);
LJ_FUNC GCtab *lj_tab_new_ah_rooted(lua_State *L, uint32_t a, uint32_t h,
				    LJTabRoot *root);
LJ_FUNC void lj_tab_root_release(LJTabRoot *root);
#if LJ_HASJIT
LJ_FUNC GCtab * LJ_FASTCALL lj_tab_new0_forjit(lua_State *L);
LJ_FUNC GCtab * LJ_FASTCALL lj_tab_new1(lua_State *L, uint32_t ahsize);
#endif
LJ_FUNCA GCtab * LJ_FASTCALL lj_tab_dup(lua_State *L, const GCtab *kt);
LJ_FUNC void LJ_FASTCALL lj_tab_clear(lua_State *L, GCtab *t);
LJ_FUNC void LJ_FASTCALL lj_tab_free(global_State *g, GCtab *t);
LJ_FUNC void lj_tab_resize(lua_State *L, GCtab *t, uint32_t asize, uint32_t hbits);
#define LJ_TAB_GC_SNAPSHOT_INVALID	0
#define LJ_TAB_GC_SNAPSHOT_OK		1
#define LJ_TAB_GC_SNAPSHOT_TRANSIENT	2
#define LJ_TAB_GC_LOOKUP_ABSENT		0
#define LJ_TAB_GC_LOOKUP_FOUND		1
#define LJ_TAB_GC_LOOKUP_RETRY		2
/* GC snapshots never manufacture semantic retention. The caller must keep an
** exact TAB object/body lease, stable root-membership lane, GC2 traversal scope,
** or terminal destructor ticket through its final returned-pointer dereference,
** plus an SMR reader or exclusive reclaimer token for separated side storage. */
LJ_FUNC int lj_tab_array_snapshot_gc_held(global_State *g, const GCtab *t,
					  TValue **arrayp, MSize *asizep,
					  MSize *acapp);
LJ_FUNC int lj_tab_node_snapshot_gc_held(global_State *g, const GCtab *t,
					 Node **nodep, MSize *hmaskp);
/* Nonwaiting string-key lookup for any caller already retaining an exact table
** scope and SMR reader. RETRY covers structural publication,
** KEYLOCK/FORWARD/claim sentinels and malformed generation chains; it must
** never be interpreted as semantic absence. */
LJ_FUNC int lj_tab_getstr_held_try(global_State *g, GCtab *t,
				   const GCstr *key, TValue *out);
/* Long C-side generation scans publish an owner-written epoch pin before
** acquiring any raw array/node pointer and drop it after the final dereference.
** Nested scopes retain the epoch of the outermost reader. */
typedef struct LJTabReadCheckpoint {
  TGState *tg;
  uint64_t epoch;
  uint32_t depth;
} LJTabReadCheckpoint;
LJ_FUNC void lj_tab_read_enter(TGState *tg);
LJ_FUNC void lj_tab_read_leave(TGState *tg);
LJ_FUNC void lj_tab_read_checkpoint(TGState *tg, LJTabReadCheckpoint *cp);
LJ_FUNC void lj_tab_read_unwind(const LJTabReadCheckpoint *cp);
/* Runtime drain only: the detached vectors require GC2's exact-thread
** exclusive-reclaimer scope; terminal cleanup calls lj_tab_freeretired(). */
LJ_FUNC uint32_t lj_tab_reclaim_retired(global_State *g,
					uint64_t completed_epoch);
LJ_FUNC void lj_tab_freeretired(global_State *g);
LJ_FUNCA void lj_tab_reasize(lua_State *L, GCtab *t, uint32_t nasize);

/* Caveat: all getters except lj_tab_get() can return NULL! */

LJ_FUNCA cTValue * LJ_FASTCALL lj_tab_getinth(GCtab *t, int32_t key);
LJ_FUNCA cTValue * LJ_FASTCALL lj_tab_getint_hop(GCtab *t, int32_t key);
LJ_FUNC cTValue *lj_tab_getstr(GCtab *t, const GCstr *key);
LJ_FUNCA cTValue *lj_tab_get(lua_State *L, GCtab *t, cTValue *key);
/* Copy one semantic value without exporting a raw vector slot. A returned GC
** value was type-validated under an exact result lease acquired before the
** vector SMR scope closed, then published while that lease remained held.
** Terminal stale snapshots are normalized to nil; callers need not repeat an
** unleased GC-header validation on out. */
LJ_FUNCA TValue *lj_tab_gettv_forjit(lua_State *L, GCtab *t, cTValue *key,
				     TValue *out);
#if defined(LJ_GC2_TEST_HELPERS)
LJ_FUNC void lj_tab_test_forjit_lease_pause(void);
LJ_FUNC uint32_t lj_tab_test_forjit_lease_paused(void);
LJ_FUNC void lj_tab_test_forjit_lease_release(void);
LJ_FUNC void lj_tab_test_forjit_snapshot_pause(void);
LJ_FUNC uint32_t lj_tab_test_forjit_snapshot_paused(void);
LJ_FUNC void lj_tab_test_forjit_snapshot_release(void);
LJ_FUNC void lj_tab_test_forjit_initial_miss_once(void);
LJ_FUNC void lj_tab_test_forjit_result_pause(void);
LJ_FUNC uint32_t lj_tab_test_forjit_result_paused(void);
LJ_FUNC void lj_tab_test_forjit_result_release(void);
#endif
/*
** Cursor traversal helpers for VM/JIT paths that hold an LJ_KEYINDEX cursor.
** They copy visible key/value snapshots into caller-owned TValue slots and
** recompute the next cursor from the current generation before returning.
** Callers must not keep raw table slots across the helper boundary; helpers
** never publish KEYLOCK/FORWARD/internal sentinels as Lua-visible results.
*/
LJ_FUNCA int32_t LJ_FASTCALL lj_tab_itern_forward(GCtab *t, uint32_t idx,
							  TValue *ctrl);
LJ_FUNCA int32_t LJ_FASTCALL lj_tab_vmnext_forward(GCtab *t, uint32_t idx,
							   TValue *out);

/* Caveat: all setters require a write barrier for the stored value. */

LJ_FUNCA TValue *lj_tab_newkey(lua_State *L, GCtab *t, cTValue *key);
LJ_FUNC int lj_tab_try_newkey_anchor(lua_State *L, GCtab *t, cTValue *key,
				     cTValue *claim, TValue **slot);
LJ_FUNC int lj_tab_try_newkey_chain(lua_State *L, GCtab *t, cTValue *key,
				    cTValue *claim, TValue **slot);
#ifdef LJ_TAB_TEST_HELPERS
typedef void (*LJTabNewkeyReserveHook)(lua_State *L, GCtab *t,
				       Node *nodebase);
typedef void (*LJTabResizeArrayHook)(lua_State *L, GCtab *t,
				     TValue *oldarray, MSize oldasize);
typedef void (*LJTabNextAfterKeyindexHook)(GCtab *t, uint32_t idx);
typedef void (*LJTabConstructorPrepublishHook)(lua_State *L, GCtab *t);
LJ_FUNC void lj_tab_test_set_newkey_anchor_after_reserve_hook(
  LJTabNewkeyReserveHook hook);
LJ_FUNC void lj_tab_test_set_newkey_chain_after_reserve_hook(
  LJTabNewkeyReserveHook hook);
LJ_FUNC void lj_tab_test_set_resize_colocated_after_freeze_hook(
  LJTabResizeArrayHook hook);
LJ_FUNC void lj_tab_test_set_next_after_keyindex_hook(
  LJTabNextAfterKeyindexHook hook);
LJ_FUNC void lj_tab_test_set_constructor_prepublish_hook(
  LJTabConstructorPrepublishHook hook);
LJ_FUNC int lj_tab_test_resize_copy_hash_slot(lua_State *L, GCtab *src,
					      MSize idx, GCtab *dst,
					      int freeze_old);
LJ_FUNC int lj_tab_test_resize_copy_array_slot(lua_State *L, GCtab *src,
					       uint32_t idx, GCtab *dst,
					       int freeze_nil_slots);
LJ_FUNC TValue *lj_tab_test_resize_assist_array_slot(lua_State *L,
						     GCtab *src,
						     uint32_t idx);
LJ_FUNC int lj_tab_test_table_candidate(global_State *g, GCobj *o);
LJ_FUNC uint32_t lj_tab_test_struct_owner_l_waits(void);
LJ_FUNC void lj_tab_test_reset_struct_owner_l_waits(void);
LJ_FUNC uint32_t lj_tab_test_struct_owner_no_l_waits(void);
LJ_FUNC void lj_tab_test_reset_struct_owner_no_l_waits(void);
LJ_FUNC uint32_t lj_tab_test_struct_enter_acquires(void);
LJ_FUNC void lj_tab_test_reset_struct_enter_acquires(void);
LJ_FUNC uint32_t lj_tab_test_new0_calls(void);
LJ_FUNC void lj_tab_test_reset_new0_calls(void);
LJ_FUNC uint32_t lj_tab_test_clear_shared_calls(void);
LJ_FUNC void lj_tab_test_reset_clear_shared_calls(void);
LJ_FUNC uint32_t lj_tab_test_tsetm_fast_calls(void);
LJ_FUNC void lj_tab_test_reset_tsetm_fast_calls(void);
LJ_FUNC uint32_t lj_tab_test_vm_array_store_calls(void);
LJ_FUNC void lj_tab_test_reset_vm_array_store_calls(void);
LJ_FUNC uint32_t lj_tab_test_vm_strhash_store_calls(void);
LJ_FUNC void lj_tab_test_reset_vm_strhash_store_calls(void);
LJ_FUNC uint32_t lj_tab_test_wait_no_l_calls(void);
LJ_FUNC void lj_tab_test_reset_wait_no_l_calls(void);
#endif
LJ_FUNCA TValue *lj_tab_setinth(lua_State *L, GCtab *t, int32_t key);
LJ_FUNC TValue *lj_tab_setint_forward(lua_State *L, GCtab *t, int32_t key);
LJ_FUNC TValue *lj_tab_setstr(lua_State *L, GCtab *t, const GCstr *key);
LJ_FUNC TValue *lj_tab_set(lua_State *L, GCtab *t, cTValue *key);
LJ_FUNCA TValue *lj_tab_storetv(lua_State *L, TValue *dst, cTValue *src);
LJ_FUNC int lj_tab_struct_enter(lua_State *L, GCtab *t);
LJ_FUNC void lj_tab_struct_leave(GCtab *t, int acquired);
LJ_FUNCA void lj_tab_wait_no_l(void);
LJ_FUNCA void lj_tab_wait_l(lua_State *L);
LJ_FUNCA void lj_tab_store_wait_l(lua_State *L);
#define LJ_TAB_STORE_CAS_OK		0
#define LJ_TAB_STORE_CAS_FORWARD	1
#define LJ_TAB_STORE_CAS_STALE		2
#define LJ_TAB_STORE_CAS_EXISTS		3
#define LJ_TAB_STORE_CAS_CHANGED	4
LJ_FUNCA int lj_tab_trystoretv_cas(lua_State *L, TValue *dst, cTValue *src);
LJ_FUNCA int lj_tab_read_current_keyed(GCtab *parent, TValue *dst,
				       cTValue *key, TValue *oldp);
LJ_FUNCA int lj_tab_trystoretv_cas_keyed(lua_State *L, GCtab *parent,
					 TValue *dst, cTValue *key,
					 cTValue *src);
LJ_FUNCA int lj_tab_trysetnil_cas_keyed(lua_State *L, GCtab *parent,
					TValue *dst, cTValue *key,
					cTValue *src, TValue *oldp);
LJ_FUNC int lj_tab_clear_weak_slot_keyed(GCtab *parent, TValue *dst,
					 cTValue *key, cTValue *val);
LJ_FUNCA TValue *lj_tab_storetv_forjit_array(lua_State *L, GCtab *parent,
					     TValue *dst, cTValue *src,
					     MSize key);
LJ_FUNCA TValue *lj_tab_storetv_forjit_array_nogc(lua_State *L,
						  GCtab *parent,
						  TValue *dst, cTValue *src,
						  MSize key);
LJ_FUNCA TValue *lj_tab_storetv_forvm_array(lua_State *L, GCtab *parent,
					    TValue *dst, cTValue *src,
					    MSize key);
LJ_FUNCA TValue *lj_tab_storetv_forvm_strhash(lua_State *L, GCtab *parent,
					      TValue *dst, cTValue *src,
					      GCstr *key);
LJ_FUNCA TValue *lj_tab_storetv_forjit_hash(lua_State *L, GCtab *parent,
					    TValue *dst, cTValue *src,
					    cTValue *key);
LJ_FUNCA TValue *lj_tab_storetv_forjit_newref(lua_State *L, GCtab *parent,
					      TValue *dst, cTValue *src,
					      cTValue *key);
LJ_FUNCA int32_t lj_tab_storetv_existing_forjit(lua_State *L, GCtab *parent,
						cTValue *key, cTValue *src);
LJ_FUNCA TValue *lj_tab_storetvn(lua_State *L, TValue *dst, cTValue *src,
				 uint32_t n);
LJ_FUNCA void lj_tab_storetvn_forvm_array(lua_State *L, GCtab *parent,
					  uint32_t start, cTValue *src,
					  uint32_t n);
LJ_FUNC TValue *lj_tab_storenilraw(TValue *dst);
LJ_FUNC TValue *lj_tab_storenil(lua_State *L, TValue *dst);
LJ_FUNC TValue *lj_tab_storebool(lua_State *L, TValue *dst, int b);
LJ_FUNC TValue *lj_tab_storeint(lua_State *L, TValue *dst, int32_t i);
LJ_FUNC TValue *lj_tab_storeintptr(lua_State *L, TValue *dst, intptr_t i);
LJ_FUNC TValue *lj_tab_storestr(lua_State *L, TValue *dst, GCstr *s);
LJ_FUNC TValue *lj_tab_storetab(lua_State *L, TValue *dst, GCtab *t);
LJ_FUNC TValue *lj_tab_storethread(lua_State *L, TValue *dst, lua_State *th);
LJ_FUNC TValue *lj_tab_storeproto(lua_State *L, TValue *dst, GCproto *pt);
LJ_FUNC TValue *lj_tab_storefunc(lua_State *L, TValue *dst, GCfunc *fn);
LJ_FUNC TValue *lj_tab_storeudata(lua_State *L, TValue *dst, GCudata *ud);
LJ_FUNC TValue *lj_tab_forwarded_array_slot(GCtab *t, TValue *array,
					    MSize asize, MSize idx,
					    TValue *valp);
LJ_FUNC TValue *lj_tab_forwarded_hash_slot(GCtab *t, Node *node, MSize hmask,
					   cTValue *key, TValue *valp);

static LJ_AINLINE int lj_tab_array_forward_hop_(const GCtab *t, TValue **arrayp,
						MSize *asizep,
						int observed_forward)
{
  TValue *array = *arrayp;
  TValue *root;
  TValue *next;
  if (!array || lj_tab_array_is_colocated(t, array))
    return 0;
  root = lj_tab_array_acq(t);
  if (root != array) {
    *asizep = lj_tab_array_snapshot_acq(t, arrayp);
    if (!*arrayp)
      return 0;
    return 1;
  }
  next = lj_tab_array_nextgen_acq(array);
  /*
  ** next_gen is installed before migration finishes. Follow it before root
  ** publication only after the caller has acquired a FORWARD slot, which is
  ** the per-slot ownership handoff marker.
  */
  if (next && next != array && !lj_tab_array_is_colocated(t, next) &&
      (lj_tab_array_acq(t) != array || observed_forward)) {
    *arrayp = next;
    *asizep = lj_tab_array_hdr_asize_acq(next);
    return 1;
  }
  return 0;
}

static LJ_AINLINE int lj_tab_array_forward_hop(const GCtab *t, TValue **arrayp,
					       MSize *asizep)
{
  return lj_tab_array_forward_hop_(t, arrayp, asizep, 0);
}

static LJ_AINLINE int lj_tab_array_forward_hop_forward(const GCtab *t,
						       TValue **arrayp,
						       MSize *asizep)
{
  return lj_tab_array_forward_hop_(t, arrayp, asizep, 1);
}

static LJ_AINLINE cTValue *lj_tab_getint(GCtab *t, int32_t key)
{
  TValue *array;
  MSize asize;
retry_array:
  asize = lj_tab_array_snapshot_acq(t, &array);
genarray:
  if ((MSize)key < asize) {
    TValue val;
    lj_tv_load_acq(&val, &array[key]);
    if (tvisforward(&val)) {
      TValue *oldarray = array;
      if (lj_tab_array_forward_hop_forward(t, &array, &asize)) {
	if ((MSize)key < asize) {
	  TValue nextval;
	  lj_tv_load_acq(&nextval, &array[key]);
	  if (tvisnil(&nextval) && lj_tab_array_acq(t) == oldarray &&
	      (lj_tab_array_is_retiring(t, oldarray) ||
	       lj_tab_array_is_colocated(t, oldarray))) {
	    lj_tab_wait_no_l();
	    goto retry_array;
	  }
	}
	goto genarray;
      }
      if (lj_tab_array_acq(t) != array ||
	  lj_tab_array_is_retiring(t, array) ||
	  lj_tab_array_is_colocated(t, array)) {
	lj_tab_wait_no_l();
	goto retry_array;
      }
      return NULL;
    }
    return &array[key];
  }
  return lj_tab_getinth(t, key);
}

static LJ_AINLINE TValue *lj_tab_setint(lua_State *L, GCtab *t, int32_t key)
{
  TValue *array;
retry_array:
  {
    MSize asize = lj_tab_array_snapshot_acq(t, &array);
  genarray:
    if ((MSize)key < asize) {
      TValue val;
      lj_tv_load_acq(&val, &array[key]);
      if (tvisforward(&val)) {
	if (lj_tab_array_forward_hop_forward(t, &array, &asize))
	  goto genarray;
	if (lj_tab_array_acq(t) != array ||
	    lj_tab_array_is_retiring(t, array) ||
	    lj_tab_array_is_colocated(t, array)) {
	  lj_tab_wait_no_l();
	  goto retry_array;
	}
	return lj_tab_setint_forward(L, t, key);
      }
      return &array[key];
    }
  }
  return lj_tab_setinth(L, t, key);
}

LJ_FUNC uint32_t LJ_FASTCALL lj_tab_keyindex(GCtab *t, cTValue *key);
LJ_FUNCA int lj_tab_next(GCtab *t, cTValue *key, TValue *o);
LJ_FUNCA MSize LJ_FASTCALL lj_tab_len(GCtab *t);
#if LJ_HASJIT
LJ_FUNC MSize LJ_FASTCALL lj_tab_len_hint(GCtab *t, size_t hint);
#endif

#endif
