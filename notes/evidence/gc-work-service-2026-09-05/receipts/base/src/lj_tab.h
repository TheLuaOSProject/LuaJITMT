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

static LJ_AINLINE TabResizeDesc *
lj_tab_resize_desc_head_acq(const global_State *g)
{
  return (TabResizeDesc *)la_loadptr_acq(
    (void *const *)&g->tab_resize.resize_descs);
}

static LJ_AINLINE int lj_tab_resize_desc_head_cas(global_State *g,
						   TabResizeDesc **oldp,
						   TabResizeDesc *desc)
{
  return la_casptr((void **)&g->tab_resize.resize_descs, (void **)oldp, desc,
		   LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE TabResizeDesc *
lj_tab_resize_desc_head_xchg_acqrel(global_State *g, TabResizeDesc *desc)
{
  return (TabResizeDesc *)la_xchgptr_acqrel(
    (void **)&g->tab_resize.resize_descs, desc);
}

static LJ_AINLINE TabResizeDesc *
lj_tab_resize_desc_next_acq(const TabResizeDesc *desc)
{
  return (TabResizeDesc *)la_loadptr_acq((void *const *)&desc->next);
}

static LJ_AINLINE void lj_tab_resize_desc_next_rel(TabResizeDesc *desc,
						    TabResizeDesc *next)
{
  la_storeptr_rel((void **)&desc->next, next);
}

static LJ_AINLINE GCtab *
lj_tab_resize_desc_tab_acq(const TabResizeDesc *desc)
{
  return (GCtab *)la_loadptr_acq((void *const *)&desc->tab);
}

static LJ_AINLINE void lj_tab_resize_desc_tab_rel(TabResizeDesc *desc,
						   GCtab *t)
{
  la_storeptr_rel((void **)&desc->tab, t);
}

static LJ_AINLINE uint64_t
lj_tab_resize_desc_id_acq(const TabResizeDesc *desc)
{
  return la_load64_acq(&desc->id);
}

static LJ_AINLINE uint32_t
lj_tab_resize_desc_phase_acq(const TabResizeDesc *desc)
{
  return la_load32_acq(&desc->phase);
}

static LJ_AINLINE uint32_t
lj_tab_resize_desc_flags_acq(const TabResizeDesc *desc)
{
  return la_load32_acq(&desc->flags);
}

static LJ_AINLINE void lj_tab_resize_desc_flags_rel(TabResizeDesc *desc,
						     uint32_t flags)
{
  la_store32_rel(&desc->flags, flags);
}

static LJ_AINLINE void lj_tab_resize_desc_flags_or(TabResizeDesc *desc,
						    uint32_t flags)
{
  uint32_t current = lj_tab_resize_desc_flags_acq(desc);
  while (!la_cas32(&desc->flags, &current, current | flags,
		   LA_ACQ_REL, LA_ACQ))
    ;
}

static LJ_AINLINE int
lj_tab_resize_desc_vm_guard_release_claim(TabResizeDesc *desc)
{
  uint32_t current = lj_tab_resize_desc_flags_acq(desc);
  for (;;) {
    if (!(current & TAB_RESIZE_DESC_F_VM_GUARD) ||
	(current & TAB_RESIZE_DESC_F_VM_GUARD_RELEASED))
      return 0;
    if (la_cas32(&desc->flags, &current,
		 current | TAB_RESIZE_DESC_F_VM_GUARD_RELEASED,
		 LA_ACQ_REL, LA_ACQ))
      return 1;
  }
}

static LJ_AINLINE int lj_tab_resize_desc_phase_cas(TabResizeDesc *desc,
						    uint32_t *oldp,
						    uint32_t phase)
{
  return la_cas32(&desc->phase, oldp, phase, LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE uint64_t
lj_tab_resize_desc_epoch_acq(const TabResizeDesc *desc)
{
  return la_load64_acq(&desc->retire_epoch);
}

static LJ_AINLINE void lj_tab_resize_desc_epoch_rel(TabResizeDesc *desc,
						     uint64_t epoch)
{
  la_store64_rel(&desc->retire_epoch, epoch);
}

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
/*
** Persistent resize descriptors are published in a global registry before any
** descriptor marker can become visible. install() leaves a PREPARED record
** private only when it cannot enter SMR. The reserve() caller exclusively owns
** that raw PREPARED pointer and may retry install(), or ask discard() to claim
** and terminalize it through the registry; discard returns zero only when SMR
** admission leaves PREPARED caller-owned. Reentrant or concurrent operations
** on the same private pointer require an outer SMR reader which begins before
** either operation. Ownership is consumed as soon as any caller wins the
** PREPARED phase CAS; raw-pointer reuse after that point is forbidden unless
** protected by an existing reader or rediscovered with find_held(). No caller
** directly frees an install-capable record. Once PREPARED is claimed, success
** leaves a borrowed active pointer and every failure leaves a terminal record
** owned by the registry. Table control is a discovery anchor, not by itself a
** pointer-lifetime lease. Terminal records stay discoverable until the same
** grace rule as retired table vectors permits physical free.
*/
LJ_FUNC TabResizeDesc *lj_tab_resize_desc_reserve(lua_State *L, GCtab *t,
						   uint32_t newacap);
LJ_FUNC int lj_tab_resize_desc_discard(global_State *g,
					TabResizeDesc *desc);
LJ_FUNC int lj_tab_resize_desc_install(lua_State *L, GCtab *t,
				       TabResizeDesc *desc);
LJ_FUNC int lj_tab_resize_desc_clear(GCtab *t, TabResizeDesc *desc,
				      uint32_t acap);
LJ_FUNC TabResizeDesc *lj_tab_resize_desc_find_held(global_State *g,
						     GCtab *t, uint64_t id);
LJ_FUNC int lj_tab_resize_desc_maintain_held(global_State *g,
					      TabResizeDesc *desc);
LJ_FUNC int lj_tab_resize_desc_advance(TabResizeDesc *desc,
				       uint32_t from, uint32_t to);
LJ_FUNC int lj_tab_resize_desc_terminal(global_State *g,
					 TabResizeDesc *desc, uint32_t from);
LJ_FUNCA void lj_tab_reasize(lua_State *L, GCtab *t, uint32_t nasize);

/* Caveat: all getters except lj_tab_get() can return NULL! */

LJ_FUNCA cTValue * LJ_FASTCALL lj_tab_getinth(GCtab *t, int32_t key);
LJ_FUNCA cTValue * LJ_FASTCALL lj_tab_getint_hop(GCtab *t, int32_t key);
LJ_FUNC cTValue *lj_tab_getstr(GCtab *t, const GCstr *key);
LJ_FUNCA cTValue *lj_tab_get(lua_State *L, GCtab *t, cTValue *key);
/* One bounded authoritative-root point read.  FOUND release-publishes the
** exact current value into |outroot|; an authority-confirmed ABSENT or RETRY
** release-publishes nil.  Invalid operands or a lost exact owner return RETRY
** without touching output, since no safe publication authority exists.
** SMR and read admission are attempted without waiting; the helper invokes
** no Lua and allocates no chain anchors.
** A successful GC-result publication can grow existing GC queue storage after
** vector SMR closes, while the exact object leases still retain every body.
** RETRY covers transient GC2
** admission, changing/retiring table generations, internal table sentinels
** and a lost exact state owner.  ABSENT includes a non-table parent, nil/NaN
** key misses, a structurally present nil slot and an ordinary missing key.
**
** All TValue pointers must name live semantic roots owned by |L| for the
** duration of the call.  Output may alias either input: exact table/key root
** confirmation and owner confirmation precede the terminal output store.
** The integer convenience form has no collectable key edge to retain or
** confirm, but otherwise provides the same contract. */
enum {
  LJ_TAB_ROOTED_GET_RETRY = -1,
  LJ_TAB_ROOTED_GET_ABSENT = 0,
  LJ_TAB_ROOTED_GET_FOUND = 1
};
LJ_FUNC int lj_tab_gettv_rooted_try(lua_State *L, cTValue *tabroot,
				     cTValue *keyroot, TValue *outroot);
LJ_FUNC int lj_tab_getinttv_rooted_try(lua_State *L, cTValue *tabroot,
					int32_t key, TValue *outroot);
/* Positive-hit form for a caller which still needs its original operands on
** a miss or admission refusal. Return one only after publishing an exactly
** retained result; return zero with all cells unchanged otherwise. Source
** cells need the same retained-container/owner/prototype provenance described
** below. Unlike the scalar form, this retains the existing one-shot SMR
** interval and supports GC results and Huge vectors. It never waits for SMR. */
LJ_FUNC int lj_tab_gettv_rooted_hit_try(lua_State *L, cTValue *tabroot,
				     cTValue *keyroot, TValue *outroot);
#if LJ_HASJIT && LJ_HASFFI
/* Native JIT comparison only. expected must be an exact typed IR_KGC cdata
** operand retained by the executing trace, not an observed raw cache pointer.
** The caller's published native interval must span the entire call. Source
** roots require the same owner/container provenance as the rooted getter.
** Keep its exact table/key leases, one-shot SMR and generation confirmation;
** return one only for the same cdata identity. No source/output store, result
** discovery lease, allocation, callback or root publication. Nonnative calls,
** misses and all source/owner/admission refusals return zero. */
LJ_FUNC int lj_tab_cmpcdata_kgc_rooted_try(lua_State *L, cTValue *tabroot,
				       cTValue *keyroot, GCcdata *expected);
#endif
/* Positive number/boolean hit only, with small table/string/vector leases
** independent of global SMR. Sources must be authoritative cells in a caller-
** retained container or stable owner/prototype roots (including VM constant
** scratch backed by the active prototype). Actor/L ownership alone does not
** establish source provenance. Output must be an enumerated owner root.
** No allocation, callback or semantic publication on failure;
** return zero with every cell unchanged for misses or unsupported cases.
** The ordinary rooted API retains its broader semantics and progress debt. */
LJ_FUNC int lj_tab_getscalar_rooted_try(lua_State *L, cTValue *tabroot,
				      cTValue *keyroot, TValue *outroot);
/* Copy one semantic value starting from an authoritative table TValue root.
** The helper retains the exact parent, key and result incarnations across its
** current-generation SMR read, and release-publishes into an already
** enumerated output root. Stack-backed inputs/outputs are restored after any
** retry wait. Inputs are snapshotted and leased before terminal output
** publication, so the output may alias either input (as required by the
** consume-key and self-key forms of lua_rawget()). */
LJ_FUNCA TValue *lj_tab_gettv_rooted(lua_State *L, cTValue *tabroot,
				     cTValue *key, TValue *outroot);
LJ_FUNCA TValue *lj_tab_getinttv_rooted(lua_State *L, cTValue *tabroot,
					int32_t key, TValue *outroot);
/* Copy one semantic value without exporting a raw vector slot. A returned GC
** value was type-validated under an exact result lease acquired before the
** vector SMR scope closed, then published while that lease remained held.
** Terminal stale snapshots are normalized to nil; callers need not repeat an
** unleased GC-header validation on out. This naked-table ABI is retained for
** existing JIT helpers and legacy C consumers which still lack a retained
** parent edge. New readers which start from a TValue root must use
** lj_tab_gettv_rooted(). */
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
typedef void (*LJTabNewkeyPublishHook)(lua_State *L, GCtab *t,
				       Node *nodebase, Node *anchor,
				       Node *claimed, uint32_t stage);
typedef void (*LJTabResizeArrayHook)(lua_State *L, GCtab *t,
				     TValue *oldarray, MSize oldasize);
typedef void (*LJTabNextAfterKeyindexHook)(GCtab *t, uint32_t idx);
typedef void (*LJTabConstructorPrepublishHook)(lua_State *L, GCtab *t);
typedef void (*LJTabStorePostCasHook)(lua_State *L, GCtab *t, TValue *dst,
				      cTValue *key, cTValue *value);
typedef void (*LJTabRootedReaderRetryHook)(lua_State *L, GCtab *t,
					   int reader);
/* Test-only one-shot hook immediately before rooted length validates its
** captured table generation. Hooks must not wait, allocate or throw. */
typedef void (*LJTabLenRootedTryHook)(GCtab *t);
typedef void (*LJTabScalarRootedTryHook)(lua_State *L, GCtab *t,
				       uint32_t stage);
enum {
  LJ_TAB_SCALAR_TEST_SOURCE = 1,
  LJ_TAB_SCALAR_TEST_VECTORS = 2,
  LJ_TAB_SCALAR_TEST_RETAINED = 3,
  LJ_TAB_SCALAR_TEST_RESULT = 4,
  LJ_TAB_SCALAR_TEST_VALUE = 5
};
LJ_FUNC void lj_tab_test_set_scalar_rooted_try_hook(
  LJTabScalarRootedTryHook hook);
/* The scalar-array next attempt shares these diagnostic stages. Hooks on that
** path must not allocate, throw, move the stack, wait or invoke Lua. Its direct
** probe returns -2=refusal, 0=end, 1=found; refusal/end preserve all outputs. */
LJ_FUNC int lj_tab_test_nextscalar_rooted_try(lua_State *L, cTValue *tabroot,
					   cTValue *keyroot, TValue *outkey,
					   TValue *outval, uint32_t *nextidx);
typedef void (*LJTabResizeDescInstallHook)(lua_State *L, GCtab *t,
					   TabResizeDesc *desc,
					   uint32_t stage);
typedef void (*LJTabResizeDescClearHook)(GCtab *t, TabResizeDesc *desc,
					 uint32_t acap);
enum {
  LJ_TAB_ROOTED_READER_NEXT = 1,
  LJ_TAB_ROOTED_READER_LEN = 2
};
enum {
  LJ_TAB_RESIZE_DESC_HOOK_BEFORE_PHASE_CAS = 1,
  LJ_TAB_RESIZE_DESC_HOOK_PUBLISHED,
  LJ_TAB_RESIZE_DESC_HOOK_BEFORE_CONTROL_CAS,
  LJ_TAB_RESIZE_DESC_HOOK_CONTROL,
  LJ_TAB_RESIZE_DESC_HOOK_CANCELLING
};
enum {
  LJ_TAB_NEWKEY_HOOK_ANCHOR_KEY = 1,
  LJ_TAB_NEWKEY_HOOK_COLLISION_KEY = 2,
  LJ_TAB_NEWKEY_HOOK_COLLISION_NEXT = 3,
  LJ_TAB_NEWKEY_HOOK_COLLISION_LINK = 4
};
LJ_FUNC void lj_tab_test_set_newkey_anchor_after_reserve_hook(
  LJTabNewkeyReserveHook hook);
LJ_FUNC void lj_tab_test_set_newkey_chain_after_reserve_hook(
  LJTabNewkeyReserveHook hook);
LJ_FUNC void lj_tab_test_set_newkey_publish_hook(
  LJTabNewkeyPublishHook hook);
LJ_FUNC void lj_tab_test_set_resize_colocated_after_freeze_hook(
  LJTabResizeArrayHook hook);
LJ_FUNC void lj_tab_test_set_next_after_keyindex_hook(
  LJTabNextAfterKeyindexHook hook);
LJ_FUNC void lj_tab_test_set_constructor_prepublish_hook(
  LJTabConstructorPrepublishHook hook);
LJ_FUNC void lj_tab_test_set_store_post_cas_hook(LJTabStorePostCasHook hook);
LJ_FUNC void lj_tab_test_keyed_cas_changed_stack_grow_once(void);
LJ_FUNC uint32_t lj_tab_test_keyed_cas_changed_stack_grow_hits(void);
LJ_FUNC void lj_tab_test_set_rooted_reader_retry_hook(
  LJTabRootedReaderRetryHook hook);
LJ_FUNC void lj_tab_test_set_len_rooted_try_hook(
  LJTabLenRootedTryHook hook);
LJ_FUNC void lj_tab_test_set_resize_desc_install_hook(
  LJTabResizeDescInstallHook hook);
LJ_FUNC void lj_tab_test_set_resize_desc_clear_hook(
  LJTabResizeDescClearHook hook);
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
LJ_FUNC uint32_t lj_tab_test_wait_l_calls(void);
LJ_FUNC void lj_tab_test_reset_wait_l_calls(void);
LJ_FUNC uint32_t lj_tab_test_store_wait_l_calls(void);
LJ_FUNC void lj_tab_test_reset_store_wait_l_calls(void);
LJ_FUNC uint32_t lj_tab_test_len_rooted_try_calls(void);
LJ_FUNC void lj_tab_test_reset_len_rooted_try_calls(void);
#endif
LJ_FUNCA TValue *lj_tab_setinth(lua_State *L, GCtab *t, int32_t key);
LJ_FUNC TValue *lj_tab_setint(lua_State *L, GCtab *t, int32_t key);
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
#define LJ_TAB_STORE_CAS_ABSENT		5
LJ_FUNCA int lj_tab_trystoretv_cas(lua_State *L, TValue *dst, cTValue *src);
LJ_FUNCA int lj_tab_read_current_keyed(global_State *g, GCtab *parent,
				       TValue *dst, cTValue *key, TValue *oldp);
LJ_FUNCA int lj_tab_trystoretv_cas_keyed(lua_State *L, GCtab *parent,
					 TValue *dst, cTValue *key,
					 cTValue *src);
LJ_FUNCA int lj_tab_trysetnil_cas_keyed(lua_State *L, GCtab *parent,
					TValue *dst, cTValue *key,
					cTValue *src, TValue *oldp);
/* oldroot is an already-enumerated TG/stack root. A competing value is
** release-published there, with its root barrier, before vector SMR is left. */
LJ_FUNCA int lj_tab_trysetnil_cas_keyed_rooted(lua_State *L, GCtab *parent,
					       TValue *dst, cTValue *key,
					       cTValue *src,
					       TValue *oldroot);
LJ_FUNC int lj_tab_clear_weak_slot_keyed(global_State *g, GCtab *parent,
					 TValue *dst, cTValue *key,
					 cTValue *val);
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

LJ_FUNC uint32_t LJ_FASTCALL lj_tab_keyindex(GCtab *t, cTValue *key);
LJ_FUNCA int lj_tab_next(GCtab *t, cTValue *key, TValue *o);
/* Generation-bound structural traversal starting from authoritative TValue
** roots. Inputs and outputs may alias as required by lua_next(); all
** stack-backed locations are restored after L-aware retry points. */
LJ_FUNCA int lj_tab_next_rooted(lua_State *L, cTValue *tabroot,
				cTValue *keyroot, TValue *outkey,
				TValue *outval, uint32_t *nextidx);
LJ_FUNCA int lj_tab_next_pair_rooted(lua_State *L, cTValue *tabroot,
				     cTValue *keyroot, TValue *out);
LJ_FUNCA int32_t LJ_FASTCALL lj_tab_itern_rooted(lua_State *L,
						 cTValue *tabroot,
						 TValue *ctrl);
LJ_FUNCA MSize LJ_FASTCALL lj_tab_len(GCtab *t);
/* One bounded length attempt from an authoritative table TValue root. The
** exact actor/TG/state owner, table root, table allocation and paired current
** structural generation must remain valid through the result linearization.
** The helper never waits, yields, allocates or throws. */
enum {
  LJ_TAB_LEN_RETRY = -1
};
LJ_FUNC int32_t lj_tab_len_rooted_try(lua_State *L, cTValue *tabroot);
/* Generated x64 active-MT length ABI. IR_TMPREF IN1 must place tabroot at the
** exact current TG tmptv. The active trace retains the table body globally;
** a caller-owned table-vector epoch retains the paired generation locally.
** Direct/interpreter calls fail closed. */
LJ_FUNC int32_t lj_tab_len_forjit_try(lua_State *L, cTValue *tabroot);
/* Bounded current-generation length search from an authoritative table root.
** The exact table and both structural vectors remain retained throughout each
** attempt; waits occur only after the SMR interval and lease are closed. */
LJ_FUNCA MSize lj_tab_len_rooted(lua_State *L, cTValue *tabroot);
#if LJ_HASJIT
LJ_FUNC MSize LJ_FASTCALL lj_tab_len_hint(GCtab *t, size_t hint);
#endif

#endif
