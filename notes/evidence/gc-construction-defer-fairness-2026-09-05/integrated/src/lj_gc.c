/*
** Garbage collector.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Major portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lj_gc_c
#define LUA_CORE

#include <stdlib.h>
#include <limits.h>

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_chan.h"
#include "lj_err.h"
#include "lj_safepoint.h"
#include "lj_thr.h"
#include "lj_tg.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_func.h"
#include "lj_udata.h"
#include "lj_meta.h"
#include "lj_debug.h"
#include "lj_state.h"
#include "lj_frame.h"
#include "lj_trace.h"
#include "lj_mcode.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#include "lj_cdata.h"
#include "lj_clib.h"
#endif
#include "lj_trace.h"
#include "lj_arena.h"

#define GCACTIVEAUTOSTEPS	64u

static GCSize gc_step_debt_quantum(global_State *g)
{
  /*
  ** Idle threshold publication stays close to the GC2 allocation trigger so a
  ** cycle starts promptly. Once GC2 is active, automatic allocation checks are
  ** only a bounded progress hook; use a larger quantum so mark fixpoint root
  ** snapshots are not retried at the idle trigger cadence.
  */
  if (gc2_phase_acq(g) != LJ_GC2_IDLE)
    return (GCSize)LJ_GC2_ACTIVE_AUTO_STEP;
  return (GCSize)LJ_GC2_HELPER_IDLE_STEP;
}

static int gc_hard_assist_due_interp(global_State *g, TGState *tg)
{
  if (!lj_gc2_hard_limit_reached(g))
    return 0;
  return !tg || lj_gc2_hard_load(g) < LJ_GC2_ACCT_FLUSH ||
	 lj_tg_local_total_acq(tg) >= LJ_GC2_ACCT_FLUSH;
}

static int gc_logical_running(global_State *g)
{
  return lj_gc_auto_running(g);
}

/* Allocation checkpoints can publish a request and consume the local debt
** before an interpreter GC check. The MT threshold bridge must not hide that
** durable request. Only a normal GC-safe caller may admit it to the existing
** driver; raw allocation accounting does not have a complete Lua stack. */
int lj_gc_pending_auto_request(global_State *g)
{
  return gc2_cycle_leader_acq(g) != 0 &&
	 gc2_phase_acq(g) == LJ_GC2_IDLE && gc_logical_running(g);
}

#if LJ_HASJIT
static int gc_hard_assist_due_jit(global_State *g)
{
  uint64_t hard, since;
  if (!lj_gc2_hard_limit_reached(g))
    return 0;
  hard = lj_gc2_hard_load(g);
  if (hard < LJ_GC2_ACCT_FLUSH)
    return 1;
  since = lj_gc2_alloc_since_load(g);
  return since >= lj_gc2_hard_check_load(g);
}
#endif

void lj_gc_fixstring(global_State *g, GCstr *s)
{
  GCobj *o;
  if (!s)
    return;
  o = obj2gco(s);
  lj_obj_addgcflags_atomic(o, LJ_GC_FIXED);
  /*
  ** Fixed strings can be referenced outside the string table by lexer token
  ** state, preallocated error paths and FFI CType names. Mark at the creation
  ** edge so GC2 does not need to rescan the whole string hash table after every
  ** fixpoint handshake to protect late fixed strings.
  */
  if (g) {
    lj_gc_arena_markobj(g, o);
    (void)lj_gc2_markobj_nogrey(g, o);
  }
}

#define isfinalized(u)		(lj_obj_gcflags(obj2gco(u)) & LJ_GC_FINALIZED)

static void gc_root_wait_no_l(void)
{
  (void)lj_thr_retry_yield(NULL);
}

static int gc_root_link_valid(global_State *g, GCobj *o);
#ifdef LJ_GC2_TEST_HELPERS
static void gc_test_root_state(global_State *g, GCobj *o, uint32_t path);
#else
static LJ_AINLINE void gc_test_root_state(global_State *g, GCobj *o,
					   uint32_t path)
{
  UNUSED(g); UNUSED(o); UNUSED(path);
}
#endif

/* -- GC2 mark bridge ----------------------------------------------------- */

void lj_gc_arena_markobj(global_State *g, GCobj *o)
{
  if (!lj_gc2_minor_roots_skip_bridge_mark(g))
    (void)lj_gc2_markobj_direct(g, o);
}

void lj_gc_arena_markmem(global_State *g, void *p)
{
  if (!lj_gc2_minor_roots_skip_bridge_mark(g))
    (void)lj_gc2_markmem_registered(g, p);
}

void lj_gc_arena_markmem_registered(global_State *g, void *p)
{
  if (!lj_gc2_minor_roots_skip_bridge_mark(g))
    (void)lj_gc2_markmem_registered(g, p);
}

static void gc2_preserve_root_spine_body(global_State *g, GCobj *o)
{
  if (o && o->gch.gct == (uint32_t)~LJ_TTAB) {
    GCtab *t = gco2tab(o);
    TValue *array = NULL;
    MSize asize = 0, acap = 0, hmask = 0;
    Node *node = NULL;
    /* The caller has just proved stable MEMBER ownership and remains the sole
    ** sweep-prune actor, which pins the table body without turning this
    ** ownership spine into a semantic root set. SMR separately pins any side
    ** generation returned by the snapshots through its mark publication. */
    lj_gc2_smr_read_enter(g);
    if (lj_tab_array_snapshot_gc_held(g, t, &array, &asize, &acap) ==
	LJ_TAB_GC_SNAPSHOT_OK && array)
      (void)lj_gc2_markmem(g, acap ? (void *)lj_tab_array_hdrw(array) :
				 (void *)array);
    UNUSED(asize);
    if (lj_tab_node_snapshot_gc_held(g, t, &node, &hmask) ==
	LJ_TAB_GC_SNAPSHOT_OK && hmask > 0)
      (void)lj_gc2_markmem(g, lj_tab_node_hdrw(node));
    lj_gc2_smr_read_leave(g);
  }
}

void lj_gc_preserve_root_chain_for_gc2_sweep(global_State *g)
{
  GCobj *o;
  uint32_t n = 0;
  (void)lj_gc_flush_root_pending(g);
  (void)lj_gc_repair_root_spine(g);
  /*
  ** The root list is still the GC root ownership spine while GC2 owns arena
  ** storage, but it is not a semantic root set. Semantic roots are closed by
  ** lj_gc2_trace_sweep_roots(); the arena owner sweep dispatches or preserves
  ** unmarked valid root-spine bodies at the bridge boundary. Do not mark child
  ** values here, but keep side-vector storage for root-spine table bodies alive
  ** for the same bridge interval as the table bodies themselves.
  */
  for (o = lj_gc_root_acq(g); o != NULL;) {
    GCobj *next;
    if (!gc_root_link_valid(g, o))
      break;
    next = lj_obj_gcw_acq(o);
    gc2_preserve_root_spine_body(g, o);
    if (next == o || ++n >= LJ_GC2_ROOT_SCAN_LIMIT)
      break;
    o = next;
  }
}

int lj_gc_tv_gcref_valid(global_State *g, cTValue *tv)
{
  return lj_gc2_tv_gcref_valid_edge(g, tv);
}

int lj_gc_tv_gcref_status(global_State *g, cTValue *tv)
{
  return lj_gc2_tv_gcref_status_edge(g, tv);
}

static MSize gc_udata_io_file_size(void)
{
  /*
  ** UDTYPE_IO_FILE has a private payload in the I/O library:
  ** { FILE *fp; uint32_t type; }.  The collector only needs the allocation
  ** size, and FILE is stored as an opaque pointer there.
  */
  return (MSize)((sizeof(void *) + sizeof(uint32_t) + sizeof(void *) - 1u) &
		 ~(sizeof(void *) - 1u));
}

int lj_gc_udata_payload_valid_as(GCudata *ud, uint8_t udtype, GCSize *sizep)
{
  MSize len;
  GCSize size;
  if (!ud || ud->gct != ~LJ_TUDATA)
    return 0;
  len = ud->len;
  if (len > LJ_MAX_UDATA || len > LJ_MAX_MEM32 - sizeof(GCudata))
    return 0;
  if (udtype >= UDTYPE__MAX)
    return 0;
  switch (udtype) {
  case UDTYPE_USERDATA:
    break;
  case UDTYPE_IO_FILE:
    if (len != gc_udata_io_file_size())
      return 0;
    break;
  case UDTYPE_FFI_CLIB:
#if LJ_HASFFI
    if (len != sizeof(CLibrary))
      return 0;
    break;
#else
    return 0;
#endif
  case UDTYPE_BUFFER:
#if LJ_HASBUFFER
    if (len != sizeof(SBufExt))
      return 0;
    break;
#else
    return 0;
#endif
  case UDTYPE_CHANNEL:
    {
      LJChan *ch = (LJChan *)uddata(ud);
      uint32_t cap, mask, rendezvous;
      uint64_t bytes;
      if (len < sizeof(LJChan))
	return 0;
      cap = la_load32_acq(&ch->cap);
      mask = la_load32_acq(&ch->mask);
      rendezvous = la_load32_acq(&ch->rendezvous);
      if (cap == 0 || (cap & (cap - 1u)) != 0 || mask != cap - 1u ||
	  rendezvous > 1u)
	return 0;
      bytes = sizeof(LJChan) + ((uint64_t)cap - 1u) * sizeof(LJChanSlot);
      if (bytes > LJ_MAX_UDATA || len != (MSize)bytes)
	return 0;
    }
    break;
  case UDTYPE_THREAD:
    {
      LJThread *th = (LJThread *)uddata(ud);
      if (len != sizeof(LJThread) ||
	  lj_thread_udata_acq(th) != ud)
	return 0;
    }
    break;
  case UDTYPE_MUTEX:
    if (len != sizeof(LJMutex))
      return 0;
    break;
  default:
    return 0;
  }
  size = sizeof(GCudata) + (GCSize)len;
  if (sizep)
    *sizep = size;
  return 1;
}

int lj_gc_udata_payload_valid(GCudata *ud, GCSize *sizep)
{
  if (!ud)
    return 0;
  return lj_gc_udata_payload_valid_as(ud, lj_udata_udtype_acq(ud), sizep);
}

static int gc_chain_splice(GCRef *p, GCobj *o)
{
  GCobj *next = lj_obj_gcw_acq(o);
  GCobj *expect = o;
  return gcref_cas(p, &expect, next);
}

static int gc_ref_cas_obj(GCRef *p, GCobj *old, GCobj *next)
{
  return gcref_cas(p, &old, next);
}

static int gc_root_link_valid(global_State *g, GCobj *o)
{
  GCobj *th;
  if (o == NULL)
    return 0;
  th = gcref_acq(*mainthread_ref(g));
  if (th && th->gch.gct == ~LJ_TTHREAD && o == th)
    return 1;
  th = gcref_acq(*vmthread_ref(g));
  if (th && th->gch.gct == ~LJ_TTHREAD && o == th)
    return 1;
  /* The ownership-spine link is the lifetime lease. Structural validation
  ** must not mark the object immediately before sweep samples liveness. */
  if (!lj_gc2_obj_valid_queued(g, o))
    return 0;
  /* Interned strings exclusively use nextgc for their hash chain and are
  ** never ownership-spine nodes. Rejecting this impossible type also closes
  ** the address-reuse ABA where a stale pending-root tail is recycled as a
  ** string before a defensive root-chain walk reaches it. */
  return la_load8_acq(&o->gch.gct) != (uint8_t)~LJ_TSTR;
}

/* The caller holds this universe's SMR reader (or exact reclaimer). Carry the
** previous small arena as a non-authoritative lookup cache; the queued
** validator still proves the exact allocation and header before any link read. */
static int gc_root_link_valid_held(global_State *g, GCobj *o,
				    void **known_arenap)
{
  LJGC2QueuedInfo info;
  GCobj *th;
  if (!o)
    return 0;
  th = gcref_acq(*mainthread_ref(g));
  if (th && th->gch.gct == ~LJ_TTHREAD && o == th) {
    if (known_arenap) *known_arenap = NULL;
    return 1;
  }
  th = gcref_acq(*vmthread_ref(g));
  if (th && th->gch.gct == ~LJ_TTHREAD && o == th) {
    if (known_arenap) *known_arenap = NULL;
    return 1;
  }
  if (known_arenap && *known_arenap &&
      lj_arena_of(o) == (GCArena *)*known_arenap) {
    GCArena *a = (GCArena *)*known_arenap;
    uint32_t cell = lj_arena_cellof(o);
    uint32_t gct;
    /* The previous node proved this exact mapped arena under the retained SMR
    ** scope. Root state is allocation-start identity and a lifetime pin, so a
    ** same-arena base needs only its local publication planes checked. Interior
    ** cdata and any invariant mismatch fall back to the complete validator. */
    if (cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS &&
	lj_arena_root_state_acq(a, cell) != LJ_ARENA_ROOT_NONE &&
	lj_arena_lifetime_state_acq(a, cell) != LJ_ARENA_LIFETIME_FREE &&
	lj_arena_bm_get(a->block, cell) && lj_arena_ready_get(a, cell)) {
      gct = (uint32_t)la_load8_acq(&o->gch.gct);
      if (gct >= (uint32_t)~LJ_TSTR && gct <= (uint32_t)~LJ_TUDATA &&
	  (gct == (uint32_t)~LJ_TCDATA) ==
	    (lj_arena_cdata_get(a, cell) != 0))
	return gct != (uint32_t)~LJ_TSTR;
    }
  }
  if (!lj_gc2_obj_queued_brief_held(g, o,
	known_arenap ? *known_arenap : NULL, &info)) {
    if (known_arenap) *known_arenap = NULL;
    return 0;
  }
  if (known_arenap)
    *known_arenap = info.start != 0 ? info.arena : NULL;
  return info.gct != (uint32_t)~LJ_TSTR;
}

typedef struct GCRootStateRef {
  GCArena *a;
  HugeTab *ht;
  void *base;
  uint32_t cell;
  uint8_t kind;
  uint8_t lifetime_state;
} GCRootStateRef;

enum {
  GC_ROOT_STATE_INVALID = 0,
  GC_ROOT_STATE_EXEMPT,
  GC_ROOT_STATE_SMALL,
  GC_ROOT_STATE_HUGE
};

static int gc_root_publish_claimed(global_State *g, GCobj *o,
				    GCRootStateRef *rootstate);

/* Resolve only an allocation base retained by its current constructor. The
** arena owner id and TLS/main-TG identity are stable under that construction
** lane, so this path cannot be vetoed by an unrelated exclusive SMR gate. */
static LJArenaAllocD *gc_constructor_allocd_at(global_State *g, void *base)
{
  uint32_t owner;
  TGState *tg;
  if (!g || !base || g->allocf != lj_arena_allocf)
    return NULL;
  owner = lj_arena_owner_acq(lj_arena_of(base));
  tg = lj_thr_get_tg();
  if (tg && tg->gl == g && lj_tg_tid_acq(tg) == owner)
    return lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL) ?
      &tg->allocd : (LJArenaAllocD *)g->allocd;
  tg = g->main_tg;
  if (tg && lj_tg_tid_acq(tg) == owner)
    return lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL) ?
      &tg->allocd : (LJArenaAllocD *)g->allocd;
  return NULL;
}

static void *gc_root_obj_base(global_State *g, GCobj *o)
{
  UNUSED(g);
#if LJ_HASFFI
  if (o && la_load8_acq(&o->gch.gct) == (uint8_t)~LJ_TCDATA) {
    /* Construction publishes this immutable header/base relation before
    ** READY. Existing-root owners pin the allocation incarnation, so base
    ** discovery must not depend on mutable CType table shape. */
    return cdataisv(gco2cd(o)) ? memcdatav(gco2cd(o)) : (void *)o;
  }
#else
  UNUSED(g);
#endif
  return o;
}

/* These call sites already retain an exact construction/root/sweep lifetime
** lane. The ordinary membership query takes its own tactical SMR reader; the
** current sweep reclaimer cannot do that because it owns the exclusive SMR
** gate, so use its non-transferable current-thread certificate instead. */
static LJ_AINLINE int gc2_mem_registered_ticketed(global_State *g,
						   const void *p)
{
  return lj_gc2_mem_registered_known(g, p) ||
	 lj_gc2_mem_registered_known_reclaim_held(g, p);
}

/* Resolve the persistent membership lane without changing semantic liveness.
** Variable cdata is keyed by its exact allocation base, while its intrusive
** root identity remains the interior GCcdata header. */
static int gc_root_state_resolve(global_State *g, GCobj *o, void *base,
				 GCRootStateRef *ref)
{
  lua_State *mainL, *vmL;
  GCArena *a;
  if (!ref)
    return GC_ROOT_STATE_INVALID;
  memset(ref, 0, sizeof(*ref));
  if (!g || !o)
    return GC_ROOT_STATE_INVALID;
  mainL = mainthread_acq(g);
  vmL = vmthread_acq(g);
  if ((mainL && o == obj2gco(mainL)) || (vmL && o == obj2gco(vmL)) ||
      g->allocf != lj_arena_allocf || la_load32_acq(&g->allocf_arena) == 0) {
    ref->kind = GC_ROOT_STATE_EXEMPT;
    ref->base = o;
    return ref->kind;
  }
  if (!base || !checkptrGC(base) || !gc2_mem_registered_ticketed(g, base))
    return GC_ROOT_STATE_INVALID;
  a = lj_arena_of(base);
  ref->a = a;
  ref->base = base;
  if (!lj_arena_ishuge(a)) {
    uint32_t cell = lj_arena_cellof(base);
    if (cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS ||
	lj_arena_cellptr(a, cell) != base)
      return GC_ROOT_STATE_INVALID;
    ref->cell = cell;
    ref->kind = GC_ROOT_STATE_SMALL;
    return ref->kind;
  } else {
    uint32_t owner = lj_arena_owner_acq(a);
    TGState *tg = lj_tg_find_owner(g, owner);
    if (!tg && g->main_tg && lj_tg_tid_acq(g->main_tg) == owner)
      tg = g->main_tg;
    if (!tg || !lj_tg_flags_test_acq(tg, TGF_HUGETAB))
      return GC_ROOT_STATE_INVALID;
    ref->ht = &tg->huge;
    ref->kind = GC_ROOT_STATE_HUGE;
    return ref->kind;
  }
}

/* Validate only after the caller owns the allocation lifetime lane (or a
** stronger subsystem token for huge mappings). In particular, block[] and
** READY are address-reuse-sensitive and must not authorize a small-object
** root CAS before LIVE->MUTATING has linearized. */
static int gc_root_state_validate_at(global_State *g, GCobj *o, void *base,
				     const GCRootStateRef *ref)
{
  if (ref->kind == GC_ROOT_STATE_EXEMPT)
    return 1;
  if (!base || base != ref->base || !checkptrGC(base) ||
      !gc2_mem_registered_ticketed(g, base))
    return 0;
#if LJ_HASFFI
  if (la_load8_acq(&o->gch.gct) == (uint8_t)~LJ_TCDATA) {
    if (cdataisv(gco2cd(o)) ? memcdatav(gco2cd(o)) != base :
			      (void *)o != base)
      return 0;
  } else
#endif
  if ((void *)o != base)
    return 0;
  if (ref->kind == GC_ROOT_STATE_SMALL)
    return lj_arena_of(base) == ref->a &&
	   lj_arena_cellof(base) == ref->cell &&
	   (lj_arena_flags_acq(ref->a) & LJ_AF_TRAVERSABLE) != 0 &&
	   lj_arena_bm_get(ref->a->block, ref->cell) &&
	   lj_arena_ready_get(ref->a, ref->cell);
  if (ref->kind == GC_ROOT_STATE_HUGE) {
    LJHugeInfo hi;
    return lj_arena_hugetab_lookup(ref->ht, base, &hi) == 1 &&
	   (hi.flags & (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY)) ==
	     (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY) &&
	   !(hi.flags & LJ_HUGEF_FREEING);
  }
  return 0;
}

static int gc_root_state_validate(global_State *g, GCobj *o,
				  const GCRootStateRef *ref)
{
  return gc_root_state_validate_at(g, o, gc_root_obj_base(g, o), ref);
}

static int gc_root_state_ref(global_State *g, GCobj *o, GCRootStateRef *ref)
{
  /* Callers hold an existing ownership-spine, construction, or terminal
  ** incarnation lease. That stronger lease makes this one pre-resolution
  ** cdata header read stable; ordinary publishers use an explicit base. */
  void *base = gc_root_obj_base(g, o);
  int kind = gc_root_state_resolve(g, o, base, ref);
  if (kind == GC_ROOT_STATE_INVALID ||
      (kind != GC_ROOT_STATE_EXEMPT &&
       !gc_root_state_validate(g, o, ref)))
    return GC_ROOT_STATE_INVALID;
  return kind;
}

static uint32_t gc_root_state_acq(const GCRootStateRef *ref)
{
  if (ref->kind == GC_ROOT_STATE_SMALL)
    return lj_arena_root_state_acq(ref->a, ref->cell);
  if (ref->kind == GC_ROOT_STATE_HUGE) {
    int state = lj_arena_hugetab_root_state_acq(ref->ht, ref->base, NULL);
    return state < 0 ? LJ_ARENA_ROOT_UNLINKING : (uint32_t)state;
  }
  return LJ_ARENA_ROOT_NONE;
}

static int gc_root_state_cas(const GCRootStateRef *ref, uint32_t from,
			     uint32_t to)
{
  if (ref->kind == GC_ROOT_STATE_SMALL)
    return lj_arena_root_state_cas(ref->a, ref->cell, from, to);
  if (ref->kind == GC_ROOT_STATE_HUGE)
    return lj_arena_hugetab_root_state_cas(
	ref->ht, ref->base, from, to, NULL);
  return ref->kind == GC_ROOT_STATE_EXEMPT;
}

static int gc_root_link_claim_at(global_State *g, GCobj *o, void *base,
				 GCRootStateRef *ref)
{
  uint32_t state;
  int kind = gc_root_state_resolve(g, o, base, ref);
  if (kind == GC_ROOT_STATE_INVALID)
    return LJ_GC_ROOT_LINK_INVALID;
  if (kind == GC_ROOT_STATE_EXEMPT)
    return LJ_GC_ROOT_LINKED;
  if (kind == GC_ROOT_STATE_SMALL) {
    /* Only the winner may inspect discovery/header state or mutate the root
    ** lane. FREE excludes stale publishers; MUTATING serializes requeue with
    ** allocator reuse without imposing a lock or wait on either side. */
    if (!lj_arena_lifetime_state_cas(ref->a, ref->cell,
				     LJ_ARENA_LIFETIME_LIVE,
				     LJ_ARENA_LIFETIME_MUTATING)) {
      /* GC2 recovery is the sole runtime DESTRUCT->RESCUE producer because it
      ** reserves globally visible traversal work before making the body
      ** readable. Ordinary root owners retain their semantic token and retry. */
      return LJ_GC_ROOT_LINK_DEFER;
    }
    ref->lifetime_state = LJ_ARENA_LIFETIME_MUTATING;
    /* External/remote free release-publishes late[] before attempting its
    ** lifetime claim. Once observed, semantic ownership was irrevocably
    ** relinquished: restore LIVE for the terminal consumer and do not inspect
    ** the header or manufacture a resurrection root. Tentative GC destruction
    ** uses DESTRUCT (without late) and is rescued only by GC2 recovery work. */
    if (lj_arena_late_get(ref->a, ref->cell)) {
      (void)lj_arena_lifetime_state_cas(ref->a, ref->cell,
					LJ_ARENA_LIFETIME_MUTATING,
					LJ_ARENA_LIFETIME_LIVE);
      ref->lifetime_state = 0;
      return LJ_GC_ROOT_LINK_DEFER;
    }
    if (!gc_root_state_validate(g, o, ref)) {
      (void)lj_arena_lifetime_state_cas(ref->a, ref->cell,
					ref->lifetime_state,
					LJ_ARENA_LIFETIME_LIVE);
      ref->lifetime_state = 0;
      return LJ_GC_ROOT_LINK_INVALID;
    }
  } else if (!gc_root_state_validate(g, o, ref)) {
    return LJ_GC_ROOT_LINK_INVALID;
  }
  state = gc_root_state_acq(ref);
  if (state == LJ_ARENA_ROOT_MEMBER) {
    if (ref->lifetime_state) {
      (void)lj_arena_lifetime_state_cas(ref->a, ref->cell,
					ref->lifetime_state,
					LJ_ARENA_LIFETIME_LIVE);
      ref->lifetime_state = 0;
    }
    return LJ_GC_ROOT_LINK_ALREADY;
  }
  if (state != LJ_ARENA_ROOT_NONE ||
      !gc_root_state_cas(ref, LJ_ARENA_ROOT_NONE,
			 LJ_ARENA_ROOT_LINKING)) {
    if (ref->lifetime_state) {
      (void)lj_arena_lifetime_state_cas(ref->a, ref->cell,
					ref->lifetime_state,
					LJ_ARENA_LIFETIME_LIVE);
      ref->lifetime_state = 0;
    }
    return LJ_GC_ROOT_LINK_DEFER;
  }
  gc_test_root_state(g, o, LJ_GC_ROOT_STATE_TEST_LINKING);
  return LJ_GC_ROOT_LINKED;
}

static int gc_root_link_commit(const GCRootStateRef *ref)
{
  if (ref->kind == GC_ROOT_STATE_EXEMPT)
    return 1;
  if (ref->kind == GC_ROOT_STATE_SMALL &&
      (!ref->lifetime_state ||
       !lj_arena_lifetime_state_cas(ref->a, ref->cell,
				    ref->lifetime_state,
				    LJ_ARENA_LIFETIME_LIVE)))
    return 0;
  /* Restore LIVE first. A racing free must then observe LINKING and leave the
  ** span alone, while the final CAS establishes the MEMBER=>LIVE invariant. */
  return gc_root_state_cas(ref, LJ_ARENA_ROOT_LINKING,
			   LJ_ARENA_ROOT_MEMBER);
}

static int gc_root_construct_claimed_at(global_State *g, GCobj *o,
					 void *base, GCRootStateRef *ref)
{
  lua_State *mainL, *vmL;
  LJArenaAllocD *ad;
  int kind;
  /* The fresh constructor retained allocator-issued base identity and already
  ** owns CONSTRUCT|LINKING. For a small arena that lane pins the mapping and
  ** incarnation, so do not make publication depend on a transient global
  ** registry reader or mutable CType shape. Huge constructors similarly use
  ** only their exact owner-side HugeTab slot. */
  if (!g || !o || !base || !ref)
    return LJ_GC_ROOT_LINK_INVALID;
  memset(ref, 0, sizeof(*ref));
  mainL = mainthread_acq(g);
  vmL = vmthread_acq(g);
  if ((mainL && o == obj2gco(mainL)) || (vmL && o == obj2gco(vmL)) ||
      g->allocf != lj_arena_allocf || la_load32_acq(&g->allocf_arena) == 0) {
    ref->kind = GC_ROOT_STATE_EXEMPT;
    ref->base = o;
    return LJ_GC_ROOT_LINKED;
  }
  if (!checkptrGC(base))
    return LJ_GC_ROOT_LINK_INVALID;
  if (!lj_arena_ishuge(lj_arena_of(base))) {
    GCArena *a = lj_arena_of(base);
    uint32_t cell = lj_arena_cellof(base);
    if (cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS ||
	lj_arena_cellptr(a, cell) != base ||
	!(lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE) ||
	!lj_arena_bm_get(a->block, cell) || !lj_arena_ready_get(a, cell))
      return LJ_GC_ROOT_LINK_INVALID;
#if LJ_HASFFI
    if (la_load8_acq(&o->gch.gct) == (uint8_t)~LJ_TCDATA) {
      if (cdataisv(gco2cd(o)) ? memcdatav(gco2cd(o)) != base :
				(void *)o != base)
	return LJ_GC_ROOT_LINK_INVALID;
    } else
#endif
    if ((void *)o != base)
      return LJ_GC_ROOT_LINK_INVALID;
    ref->a = a;
    ref->base = base;
    ref->cell = cell;
    ref->kind = GC_ROOT_STATE_SMALL;
    kind = GC_ROOT_STATE_SMALL;
  } else {
    LJHugeInfo hi;
    /* A fresh huge constructor is recorded in its allocating TG's HugeTab
    ** before any payload byte is returned. Resolve only that exact owner-side
    ** slot; a process-wide registry reader may be unavailable while an
    ** unrelated thread owns the exclusive SMR gate. */
    ad = gc_constructor_allocd_at(g, base);
    if (!ad || !ad->huge ||
	lj_arena_hugetab_lookup(ad->huge, base, &hi) != 1 ||
	(hi.flags & (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY)) !=
	  (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY) ||
	(hi.flags & LJ_HUGEF_FREEING))
      return LJ_GC_ROOT_LINK_INVALID;
#if LJ_HASFFI
    if (la_load8_acq(&o->gch.gct) == (uint8_t)~LJ_TCDATA) {
      if (cdataisv(gco2cd(o)) ? memcdatav(gco2cd(o)) != base :
				(void *)o != base)
	return LJ_GC_ROOT_LINK_INVALID;
    } else
#endif
    if ((void *)o != base)
      return LJ_GC_ROOT_LINK_INVALID;
    ref->a = lj_arena_of(base);
    ref->ht = ad->huge;
    ref->base = base;
    ref->kind = GC_ROOT_STATE_HUGE;
    kind = GC_ROOT_STATE_HUGE;
  }
  if (gc_root_state_acq(ref) != LJ_ARENA_ROOT_LINKING)
    return LJ_GC_ROOT_LINK_DEFER;
  if (kind == GC_ROOT_STATE_SMALL) {
    uint32_t life = lj_arena_lifetime_state_acq(ref->a, ref->cell);
    if (life != LJ_ARENA_LIFETIME_CONSTRUCT &&
	life != LJ_ARENA_LIFETIME_RECOVERY)
      return LJ_GC_ROOT_LINK_DEFER;
  }
  return LJ_GC_ROOT_LINKED;
}

static int gc_root_construct_commit(const GCRootStateRef *ref)
{
  if (ref->kind == GC_ROOT_STATE_EXEMPT)
    return 1;
  if (ref->kind == GC_ROOT_STATE_SMALL)
    return lj_arena_root_construct_commit(ref->a, ref->cell);
  if (ref->kind == GC_ROOT_STATE_HUGE)
    return lj_arena_hugetab_root_construct_commit(
	ref->ht, ref->base, NULL) == LJ_ARENA_HUGE_ROOT_COMPLETE_LIVE;
  return 0;
}

static int gc_root_clear_complete(global_State *g,
				  const GCRootStateRef *ref, uint32_t from)
{
  if (ref->kind == GC_ROOT_STATE_EXEMPT)
    return 1;
  if (ref->kind == GC_ROOT_STATE_SMALL) {
    int completed = gc_root_state_cas(ref, from, LJ_ARENA_ROOT_NONE);
    if (completed)
      /* A concurrent free records late[] while the root state pins the span.
      ** Clearing the last claim makes that remembered free actionable. */
      lj_arena_recovery_complete_wake(ref->a);
    return completed;
  }
  if (ref->kind == GC_ROOT_STATE_HUGE) {
    int completed = lj_arena_hugetab_root_complete(
	ref->ht, ref->base, from, LJ_ARENA_ROOT_NONE,
	g ? lj_gc2_retire_epoch(g) : 0, NULL);
    if (completed == LJ_ARENA_HUGE_ROOT_COMPLETE_SWEEP) {
      /* The metadata CAS folded a racing logical free into FREEING|SWEEP_OLD
      ** with a fresh epoch. Ensure the parked sweep owner observes it. */
      gc2_sweep_grace_needed_rel(g, 1);
      lj_gc2_sweep_publish_wake(g);
    }
    return completed != LJ_ARENA_HUGE_ROOT_COMPLETE_LOST;
  }
  return 0;
}

static void gc_root_link_rollback(global_State *g,
				  const GCRootStateRef *ref)
{
  if (ref->kind == GC_ROOT_STATE_SMALL && ref->lifetime_state) {
    int cleared = gc_root_state_cas(ref, LJ_ARENA_ROOT_LINKING,
				    LJ_ARENA_ROOT_NONE);
    int restored = cleared &&
      lj_arena_lifetime_state_cas(ref->a, ref->cell,
				  ref->lifetime_state,
				  LJ_ARENA_LIFETIME_LIVE);
    lj_assertG(restored, "ordinary root-link rollback lost lifetime");
    UNUSED(restored);
    return;
  }
  (void)gc_root_clear_complete(g, ref, LJ_ARENA_ROOT_LINKING);
}

/* Terminal/shutdown reconciliation has no surviving publisher, so it can
** identify which owner protocol was interrupted from the lifetime plane. */
static int gc_root_link_terminal_rollback(global_State *g,
					  const GCRootStateRef *ref)
{
  if (ref->kind == GC_ROOT_STATE_SMALL) {
    uint32_t life = lj_arena_lifetime_state_acq(ref->a, ref->cell);
    if (life == LJ_ARENA_LIFETIME_CONSTRUCT)
      return lj_arena_root_construct_abandon(ref->a, ref->cell);
    if (life == LJ_ARENA_LIFETIME_MUTATING ||
	life == LJ_ARENA_LIFETIME_RECOVERY ||
	life == LJ_ARENA_LIFETIME_RESCUE) {
      if (!gc_root_state_cas(ref, LJ_ARENA_ROOT_LINKING,
			     LJ_ARENA_ROOT_NONE))
	return 0;
      return lj_arena_lifetime_state_cas(ref->a, ref->cell,
					 life,
					 LJ_ARENA_LIFETIME_LIVE);
    }
  }
  return gc_root_clear_complete(g, ref, LJ_ARENA_ROOT_LINKING);
}

static int gc_root_unlink_claim(global_State *g, GCobj *o,
				GCRootStateRef *ref)
{
  uint32_t state;
  int kind = gc_root_state_ref(g, o, ref);
  if (kind == GC_ROOT_STATE_INVALID)
    return LJ_GC_ROOT_UNLINK_UNPROVEN;
  if (kind == GC_ROOT_STATE_EXEMPT)
    return LJ_GC_ROOT_UNLINKED;
  state = gc_root_state_acq(ref);
  if (state == LJ_ARENA_ROOT_NONE)
    return LJ_GC_ROOT_UNLINK_ABSENT;
  if (state != LJ_ARENA_ROOT_MEMBER ||
      !gc_root_state_cas(ref, LJ_ARENA_ROOT_MEMBER,
			 LJ_ARENA_ROOT_UNLINKING))
    return LJ_GC_ROOT_UNLINK_UNPROVEN;
  gc_test_root_state(g, o, LJ_GC_ROOT_STATE_TEST_UNLINKING);
  return LJ_GC_ROOT_UNLINKED;
}

/* queued_info was produced while the root MEMBER lane and the caller's SMR
** scope still pin this exact small allocation. Reuse that proof instead of
** resolving the same registry/base/header identity for the unlink CAS. */
static int gc_root_unlink_claim_queued(global_State *g, GCobj *o,
				       const LJGC2QueuedInfo *info,
				       GCRootStateRef *ref)
{
  uint32_t state;
  GCArena *a = info ? (GCArena *)info->arena : NULL;
  if (!a || !info->base || lj_arena_ishuge(a) ||
      info->start < LJ_AFIRST_CELL || info->start >= LJ_ARENA_CELLS) {
    return gc_root_unlink_claim(g, o, ref);
  }
  memset(ref, 0, sizeof(*ref));
  ref->a = a;
  ref->base = info->base;
  ref->cell = info->start;
  ref->kind = GC_ROOT_STATE_SMALL;
  state = gc_root_state_acq(ref);
  if (state == LJ_ARENA_ROOT_NONE)
    return LJ_GC_ROOT_UNLINK_ABSENT;
  if (state != LJ_ARENA_ROOT_MEMBER ||
      !gc_root_state_cas(ref, LJ_ARENA_ROOT_MEMBER,
			 LJ_ARENA_ROOT_UNLINKING))
    return LJ_GC_ROOT_UNLINK_UNPROVEN;
  gc_test_root_state(g, o, LJ_GC_ROOT_STATE_TEST_UNLINKING);
  return LJ_GC_ROOT_UNLINKED;
}

static void gc_root_unlink_restore(const GCRootStateRef *ref)
{
  if (ref->kind != GC_ROOT_STATE_EXEMPT)
    (void)gc_root_state_cas(ref, LJ_ARENA_ROOT_UNLINKING,
			   LJ_ARENA_ROOT_MEMBER);
}

static int gc_root_unlink_commit(global_State *g,
				 const GCRootStateRef *ref)
{
  return gc_root_clear_complete(g, ref, LJ_ARENA_ROOT_UNLINKING);
}

static int gc_root_chain_break_cycle_held(global_State *g, GCobj *head)
{
  GCobj *tortoise, *hare, *slow, *fast, *entry, *tail, *next;
  void *arena = NULL, *slow_arena = NULL, *fast_arena = NULL;
  uint32_t power = 1, lam = 1, i;
  if (!head || !gc_root_link_valid_held(g, head, &arena))
    return 0;
  tortoise = head;
  hare = lj_obj_gcw_acq(head);
  if (!hare)
    return 0;
  /* The ordinary no-cycle case validates each ownership entry exactly once.
  ** Keep the more expensive two-cursor relocation only for a detected cycle. */
  while (tortoise != hare) {
    if (!gc_root_link_valid_held(g, hare, &arena))
      return 0;
    next = lj_obj_gcw_acq(hare);
    if (!next)
      return 0;
    if (power == lam) {
      tortoise = hare;
      power = power > ~(uint32_t)0 / 2u ? ~(uint32_t)0 : power << 1;
      lam = 0;
    }
    hare = next;
    if (lam != ~(uint32_t)0)
      lam++;
  }

  /* Brent's distance is the cycle length. Relocate its entry under the same
  ** reader; revalidation here is cold-path protection against concurrent
  ** splices changing a link after the initial sequential scan. */
  slow = head;
  fast = head;
  for (i = 0; i < lam; i++) {
    if (!gc_root_link_valid_held(g, fast, &fast_arena))
      return 0;
    fast = lj_obj_gcw_acq(fast);
    if (!fast)
      return 0;
  }
  while (slow != fast) {
    if (!gc_root_link_valid_held(g, slow, &slow_arena) ||
	!gc_root_link_valid_held(g, fast, &fast_arena))
      return 0;
    slow = lj_obj_gcw_acq(slow);
    fast = lj_obj_gcw_acq(fast);
  }
  entry = slow;
  tail = entry;
  /*
  ** Arena cells can be reused while an old root-spine entry for the same
  ** address is still visible to lock-free publishers. Splicing a pending chain
  ** that contains the reused address back to the old spine forms
  ** head..tail -> oldhead..entry. Sever the cycle predecessor with the same
  ** GCRef CAS discipline used by root unlinking, preserving each unique object
  ** reachable from head exactly once.
  */
  while ((next = lj_obj_gcw_acq(tail)) != entry) {
    if (next == NULL || !gc_root_link_valid_held(g, next, &arena))
      return 0;
    tail = next;
  }
  while (lj_obj_gcw_acq(tail) == entry) {
    if (gc_ref_cas_obj(lj_obj_gcwref(tail), entry, NULL))
      return 1;
    gc_root_wait_no_l();
  }
  return 0;
}

static int gc_root_chain_break_cycle(global_State *g, GCobj *head)
{
  int fixed;
  int reclaim_held = lj_gc2_reclaim_context_held(g);
  if (!reclaim_held && !lj_gc2_smr_read_try(g))
    return 0;
  fixed = gc_root_chain_break_cycle_held(g, head);
  if (!reclaim_held)
    lj_gc2_smr_read_leave(g);
  return fixed;
}

uint32_t lj_gc_repair_root_spine(global_State *g)
{
  uint64_t epoch, repaired;
  int fixed;
  if (!g)
    return 0;
  epoch = lj_gcroot_repair_epoch_acq(g);
  repaired = lj_gcroot_repaired_epoch_acq(g);
  if (repaired == epoch)
    return 0;
  fixed = gc_root_chain_break_cycle(g, lj_gc_root_acq(g));
  /*
  ** Root-spine cycles can only be introduced by root-spine publication. The
  ** publish CAS/release store orders the links themselves; this epoch is only a
  ** conservative cache of whether a scan has already covered that publication.
  ** If another publisher bumps the epoch while this scan runs, storing the old
  ** epoch leaves the newer publication visible to the next repair.
  */
  lj_gcroot_repaired_epoch_rel(g, epoch);
  return (uint32_t)fixed;
}

/* -- Sweep phase --------------------------------------------------------- */

/* Type of GC free functions. */
typedef void (LJ_FASTCALL *GCFreeFunc)(global_State *g, GCobj *o);

/* GC free functions for LJ_TSTR .. LJ_TUDATA. ORDER LJ_T */
static const GCFreeFunc gc_freefunc[] = {
  (GCFreeFunc)lj_str_free,
  (GCFreeFunc)lj_func_freeuv,
  (GCFreeFunc)lj_state_free,
  (GCFreeFunc)lj_func_freeproto,
  (GCFreeFunc)lj_func_free,
#if LJ_HASJIT
  (GCFreeFunc)lj_trace_free,
#else
  (GCFreeFunc)0,
#endif
#if LJ_HASFFI
  (GCFreeFunc)lj_cdata_free,
#else
  (GCFreeFunc)0,
#endif
  (GCFreeFunc)lj_tab_free,
  (GCFreeFunc)lj_udata_free
};

static int gc2_free_body_info(global_State *g, GCobj *o, void **basep,
			      GCSize *sizep);
static int gc_destructor_enter_impl(global_State *g, void *base, GCSize size,
				    LJGCDestructCtx *ctx,
				    int reclaim_held);

static int gc2_free_unmarked_obj(global_State *g, GCobj *o,
				 int huge_preowned, int reclaim_held)
{
  uint32_t gct = o->gch.gct;
  /*
  ** Strings are owned by the intern table and swept by GC2's string-table
  ** owner (or the terminal GC2 shutdown drain).
  ** Traversable arena scans can encounter stale type bytes from reclaimed or
  ** reused bodies; dispatching those through lj_str_free() would double-count
  ** string-table ownership and corrupt g->str.num.
  */
  if (gct == (uint32_t)~LJ_TSTR)
    return 0;
#if LJ_HASJIT
  if (gct == (uint32_t)~LJ_TTRACE) {
    /* This is semantic retirement, not the physical type destructor. The JIT
    ** token excludes assembler/link publication while retire_epoch/list/slot
    ** ownership is transferred, and the intact body remains SMR-readable.
    ** trace_freebody()/trace_free_immediate() independently acquire the exact
    ** DESTRUCT->FREE (or HugeTab BUSY) LP before exittab, gct, accounting or
    ** allocation bytes are mutated. Root UNLINKING deliberately prevents that
    ** physical LP during the earlier token transaction. */
    return lj_trace_free_gc(g, gco2trace(o)) ?
	   LJ_GC_DESTRUCT_ACQUIRED : LJ_GC_DESTRUCT_LOST;
  }
#endif
  if (gct >= (uint32_t)~LJ_TSTR && gct <= (uint32_t)~LJ_TUDATA) {
    GCFreeFunc fn = gc_freefunc[gct - (uint32_t)~LJ_TSTR];
    if (fn) {
      LJGCDestructCtx dctx;
      void *base;
      GCSize size;
      int acquired, thread_gcprep = 0;
      if (!gc2_free_body_info(g, o, &base, &size))
	return LJ_GC_DESTRUCT_LOST;
      /* Huge reclaim calls only after its full-slot RETIRED->FREEING CAS.
      ** Every other path acquires small DESTRUCT->FREE or a huge BUSY claim
      ** here, before the type-specific function can mutate side allocations,
      ** object bytes, or global accounting. */
      if (reclaim_held && gct == (uint32_t)~LJ_TTHREAD) {
	/* lua_State is fixed-size small-arena storage. Its semantic destructor
	** closes upvalues and touches global callback/registry state, so transfer
	** that work out of the exclusive writer only after reserving an explicit
	** arena completion pin and winning the irreversible destructor LP. */
	if (LJ_UNLIKELY(huge_preowned || lj_arena_ishuge(lj_arena_of(base)))) {
	  lj_assertG(0, "huge lua_State reached terminal preparation");
	  abort();
	}
	if (!lj_state_gcprep_claim_and_pin(g, gco2th(o)))
	  return LJ_GC_DESTRUCT_LOST;
	thread_gcprep = 1;
      }
      if (huge_preowned) {
        GCArena *a = lj_arena_of(base);
        TGState *tg = lj_tg_find_owner(g, lj_arena_owner_acq(a));
        LJHugeInfo hi;
        if (!lj_arena_ishuge(a) || !tg ||
	    !lj_tg_flags_test_acq(tg, TGF_HUGETAB) ||
	    lj_arena_hugetab_lookup(&tg->huge, base, &hi) != 1 ||
	    !(hi.flags & LJ_HUGEF_FREEING))
	  return LJ_GC_DESTRUCT_LOST;
	acquired = LJ_GC_DESTRUCT_ACQUIRED;
      } else {
        acquired = gc_destructor_enter_impl(g, base, size, &dctx,
					     reclaim_held);
      }
      if (acquired != LJ_GC_DESTRUCT_ACQUIRED) {
	if (thread_gcprep)
	  lj_state_gcprep_cancel(g, gco2th(o));
	return acquired;
      }
      if (thread_gcprep) {
	lj_state_gcprep_publish(g, gco2th(o));
	return LJ_GC_DESTRUCT_ACQUIRED;
      }
      fn(g, o);
      if (!huge_preowned)
	lj_gc_destructor_leave(g, &dctx);
      return LJ_GC_DESTRUCT_ACQUIRED;
    }
  }
  return LJ_GC_DESTRUCT_LOST;
}

#ifdef LJ_GC2_TEST_HELPERS
int lj_gc_test_reclaim_thread(global_State *g, lua_State *L)
{
  if (!g || !L || !lj_gc2_reclaim_context_held(g) ||
      L == mainthread_acq(g) || L->gct != (uint32_t)~LJ_TTHREAD ||
      mref(L->glref, global_State) != g)
    return LJ_GC_DESTRUCT_LOST;
  return gc2_free_unmarked_obj(g, obj2gco(L), 0, 1);
}
#endif

static int gc2_size_fits_mem(global_State *g, const void *p, GCSize size)
{
  GCArena *a;
  uint32_t cell, end, ncells;
  if (!g || g->allocf != lj_arena_allocf)
    return 1;
  if (!p || size > LJ_MAX_MEM32)
    return 0;
  /* Prove mapping ownership and an exact live allocation start before reading
  ** its arena header. Destructor validation can receive stale side pointers. */
  if (!gc2_mem_registered_ticketed(g, p))
    return 0;
  a = lj_arena_of(p);
  if (lj_arena_ishuge(a)) {
    TGState *tg = lj_tg_find_owner(g, lj_arena_owner_acq(a));
    LJHugeInfo hi;
    return tg && lj_tg_flags_test_acq(tg, TGF_HUGETAB) &&
	   lj_arena_hugetab_lookup(&tg->huge, p, &hi) == 1 &&
	   !(hi.flags & LJ_HUGEF_FREEING) && size <= hi.size;
  }
  cell = lj_arena_cellof(p);
  if (cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS)
    return 0;
  ncells = lj_arena_ncells(size);
  /* block|mark is the allocation/free boundary map. RESET_ALLOC publishes the
  ** old bump tail before root pruning, so the next boundary is authoritative
  ** for every object whose destructor GC2 may dispatch. */
  for (end = cell + 1u; end < LJ_ARENA_CELLS; end++)
    if (lj_arena_bm_get(a->block, end) || lj_arena_bm_get(a->mark, end))
      break;
  return ncells <= end - cell;
}

static int gc2_size_fits_arena(global_State *g, GCobj *o, GCSize size)
{
  return gc2_size_fits_mem(g, o, size);
}

static int gc2_valid_func_free_obj(global_State *g, GCfunc *fn)
{
  GCSize size;
  if (!fn || !checkptrGC(fn) ||
      (((uintptr_t)fn & (uintptr_t)(sizeof(void *) - 1u)) != 0) ||
      !lj_gc2_obj_valid_queued(g, obj2gco(fn)) || fn->c.gct != ~LJ_TFUNC)
    return 0;
  /* Every caller retains one of: root MEMBER/UNLINKING, a held small-arena
  ** DESTRUCT lane, a post-grace RETIRED ticket, a HugeTab TICKET/FREEING claim,
  ** or joined-world terminal ownership. The queued validator deliberately
  ** consumes that stronger lease without opening and immediately dropping a
  ** transient rescue scope before the nupvalue/size dereferences below. */
  if (isluafunc(fn)) {
    uint32_t nup = lj_funcL_nupvalues(&fn->l);
    if (nup > LJ_MAX_UPVAL)
      return 0;
    size = (GCSize)sizeLfunc((MSize)nup);
  } else {
    size = (GCSize)sizeCfunc((MSize)lj_funcC_nupvalues(&fn->c));
  }
  return gc2_size_fits_arena(g, obj2gco(fn), size);
}

static int gc2_valid_proto_obj(global_State *g, GCproto *pt)
{
  MSize minpt;
  if (pt->sizept < sizeof(GCproto) || pt->sizept > LJ_MAX_MEM32)
    return 0;
  if (!gc2_size_fits_arena(g, obj2gco(pt), pt->sizept))
    return 0;
  if (pt->sizebc == 0 || pt->sizebc > LJ_MAX_BCINS)
    return 0;
  if (pt->framesize > LJ_MAX_SLOTS || pt->sizeuv > LJ_MAX_UPVAL)
    return 0;
  if ((uintptr_t)mref(pt->jit_startins, void) !=
      (uintptr_t)(void *)(proto_bc(pt) + pt->sizebc))
    return 0;
  minpt = (MSize)sizeof(GCproto) + pt->sizebc*2u*(MSize)sizeof(BCIns);
  minpt = (minpt + (MSize)sizeof(TValue)-1) & ~((MSize)sizeof(TValue)-1);
  return minpt <= pt->sizept;
}

static int gc2_valid_pow2_mask(MSize hmask)
{
  MSize hsize;
  if (hmask == 0)
    return 1;
  if (hmask > (((MSize)1u << LJ_MAX_HBITS) - 1u))
    return 0;
  hsize = hmask + 1u;
  return (hsize & hmask) == 0;
}

static int gc2_valid_tab_obj(global_State *g, GCtab *t,
			     int vector_reclaim_owned)
{
  /* Every caller owns the retained root/small RETIRED/huge ticket/terminal
  ** certificate documented by gc2_valid_freeable_obj(). A physical-reclaim or
  ** joined-world caller also owns side-vector reclaim; earlier root pruning
  ** takes an opportunistic SMR reader instead and retries if reclaim is active. */
  int smr_reader = 0;
  int valid = 0;
  int8_t colo = lj_tab_colo_acq(t);
  MSize colosz = lj_tab_colo_size(t);
  MSize asize, acap;
  TValue *array;
  TValue *coloarray = (TValue *)(void *)((char *)(void *)t + sizeof(GCtab));
  Node *node;
  MSize hmask;
  GCSize bodysize;

  if (!vector_reclaim_owned) {
    if (!lj_gc2_smr_read_try(g))
      return 0;
    smr_reader = 1;
  }

  if (lj_tab_array_snapshot_gc_held(g, t, &array, &asize, &acap) !=
      LJ_TAB_GC_SNAPSHOT_OK ||
      lj_tab_node_snapshot_gc_held(g, t, &node, &hmask) !=
      LJ_TAB_GC_SNAPSHOT_OK)
    goto out;
  if (LJ_MAX_COLOSIZE != 0 && colosz > LJ_MAX_COLOSIZE)
    goto out;
  if (asize > LJ_MAX_ASIZE || acap > LJ_MAX_ASIZE)
    goto out;
  if (colo > 0) {
    if (array != coloarray || asize > colosz || lj_tab_acap_acq(t) > colosz)
      goto out;
  } else if (array == coloarray) {
    /*
    ** A negative colocated marker means a resize has split the old inline array
    ** from table indexing. A dead table still pointing at the inline storage is
    ** in a transient resize state; ordinary sweep must not free it as separated.
    */
    goto out;
  } else if (array != NULL) {
    if (!gc2_size_fits_mem(g, lj_tab_array_hdrw(array),
			   lj_tab_array_bytes(acap)))
      goto out;
  }

  if (node == NULL)
    goto out;
  if (!gc2_valid_pow2_mask(hmask))
    goto out;
  if (hmask > 0) {
    if (!gc2_size_fits_mem(g, lj_tab_node_hdrw(node),
			   lj_tab_node_bytes(hmask)))
      goto out;
  }

  bodysize = (LJ_MAX_COLOSIZE != 0 && colosz) ?
	     (GCSize)sizetabcolo(colosz) : (GCSize)sizeof(GCtab);
  valid = gc2_size_fits_arena(g, obj2gco(t), bodysize);
out:
  if (smr_reader)
    lj_gc2_smr_read_leave(g);
  return valid;
}

static int gc2_cdata_finalizer_pending(GCobj *o)
{
#if LJ_HASFFI
  return o->gch.gct == (uint32_t)~LJ_TCDATA &&
	 (lj_obj_gcflags(o) & LJ_GC_CDATA_FIN);
#else
  UNUSED(o);
  return 0;
#endif
}

static int gc2_udata_finalizer_pending(GCobj *o)
{
  return o->gch.gct == (uint32_t)~LJ_TUDATA &&
	 (lj_obj_gcflags(o) & LJ_GC_UDATA_FINREG) != 0;
}

static int gc2_valid_freeable_obj(global_State *g, GCobj *o,
				  int vector_reclaim_owned)
{
  uint32_t gct = o->gch.gct;
  if (gct == (uint32_t)~LJ_TSTR)
    return 0;  /* String table sweep owns GCstr lifetime. */
  if (gc2_cdata_finalizer_pending(o))
    return 0;  /* FINREG dispatch must clear LJ_GC_CDATA_FIN before free. */
#if LJ_HASFFI
  if (gct == (uint32_t)~LJ_TCDATA &&
      !lj_gc2_obj_valid_queued(g, o))
    return 0;  /* Stale cdata header: ctype/size is not safe to dispatch. */
  /* gc2_valid_freeable_obj() is reached only with the same retained root,
  ** sweep-ticket, HugeTab-ticket, or terminal lease documented for the
  ** function validator above. cdata base/size validation remains inside that
  ** lease until the central destructor acquisition. */
#endif
  if (gct == (uint32_t)~LJ_TPROTO && !gc2_valid_proto_obj(g, gco2pt(o)))
    return 0;  /* Stale proto header: sizept is not safe for destructor. */
  if (gct == (uint32_t)~LJ_TFUNC && !gc2_valid_func_free_obj(g, gco2func(o)))
    return 0;  /* Free only needs a sane closure body; traversal stays strict. */
  if (gct == (uint32_t)~LJ_TTHREAD &&
      mref(gco2th(o)->glref, global_State) != g)
    return 0;  /* Stale thread header: glref is not safe for destructor. */
  if (gct == (uint32_t)~LJ_TTAB &&
      !gc2_valid_tab_obj(g, gco2tab(o), vector_reclaim_owned))
    return 0;  /* Stale table header: side-vector sizes are not trustworthy. */
  if (gct == (uint32_t)~LJ_TUDATA) {
    GCSize size;
    if (!lj_gc_udata_payload_valid(gco2ud(o), &size) ||
	!gc2_size_fits_arena(g, o, size))
      return 0;  /* Stale userdata header: payload size is not trustworthy. */
    if (lj_udata_udtype_acq(gco2ud(o)) == UDTYPE_THREAD &&
	lj_thread_live_node_acq((LJThread *)uddata(gco2ud(o))) != NULL)
      return 0;  /* Published native root still owns this thread userdata. */
  }
  return gct >= (uint32_t)~LJ_TSTR && gct <= (uint32_t)~LJ_TUDATA &&
	 gc_freefunc[gct - (uint32_t)~LJ_TSTR] != NULL;
}

static int gc2_valid_freeable_queued(global_State *g, GCobj *o,
				     const LJGC2QueuedInfo *info)
{
  uint32_t gct = info ? info->gct : 0;
  if (info && info->arena && info->base == (void *)o &&
      !lj_arena_ishuge((GCArena *)info->arena)) {
    if (gct == (uint32_t)~LJ_TUPVAL)
      return info->alloc_size >= sizeof(GCupval);
    if (gct == (uint32_t)~LJ_TFUNC) {
      GCfunc *fn = gco2func(o);
      MSize nup = isluafunc(fn) ?
	lj_funcL_nupvalues(&fn->l) : lj_funcC_nupvalues(&fn->c);
      GCSize size;
      if (nup > LJ_MAX_UPVAL)
	return 0;
      size = isluafunc(fn) ? (GCSize)sizeLfunc(nup) :
			     (GCSize)sizeCfunc(nup);
      return (size_t)size <= info->alloc_size;
    }
  }
  return gc2_valid_freeable_obj(g, o, 0);
}

/* Resolve the exact allocation start and body extent using only validated,
** immutable destructor metadata. This may read a still-live body, but performs
** no mutation; lj_gc_destructor_enter() is the mandatory next operation. */
static int gc2_free_body_info(global_State *g, GCobj *o, void **basep,
			      GCSize *sizep)
{
  uint32_t gct;
  void *base = o;
  GCSize size;
  if (!g || !o || !basep || !sizep)
    return 0;
  gct = o->gch.gct;
  if (gct == (uint32_t)~LJ_TUPVAL) {
    size = (GCSize)sizeof(GCupval);
  } else if (gct == (uint32_t)~LJ_TTHREAD) {
    size = (GCSize)sizeof(lua_State);
  } else if (gct == (uint32_t)~LJ_TPROTO) {
    size = gco2pt(o)->sizept;
  } else if (gct == (uint32_t)~LJ_TFUNC) {
    GCfunc *fn = gco2func(o);
    size = isluafunc(fn) ?
	   (GCSize)sizeLfunc((MSize)lj_funcL_nupvalues(&fn->l)) :
	   (GCSize)sizeCfunc((MSize)lj_funcC_nupvalues(&fn->c));
#if LJ_HASFFI
  } else if (gct == (uint32_t)~LJ_TCDATA) {
    if (!lj_cdata_validate(g, gco2cd(o), &base, &size))
      return 0;
#endif
  } else if (gct == (uint32_t)~LJ_TTAB) {
    MSize colosz = lj_tab_colo_size(gco2tab(o));
    size = (LJ_MAX_COLOSIZE != 0 && colosz) ?
	   (GCSize)sizetabcolo(colosz) : (GCSize)sizeof(GCtab);
  } else if (gct == (uint32_t)~LJ_TUDATA) {
    if (!lj_gc_udata_payload_valid(gco2ud(o), &size))
      return 0;
  } else {
    return 0;
  }
  if (g->allocf == lj_arena_allocf &&
      la_load32_acq(&g->allocf_arena) != 0 &&
      gc2_mem_registered_ticketed(g, base) &&
      lj_arena_ishuge(lj_arena_of(base))) {
    GCArena *a = lj_arena_of(base);
    TGState *tg = lj_tg_find_owner(g, lj_arena_owner_acq(a));
    LJHugeInfo hi;
    /* The semantic retire owner or the immediately preceding full-slot claim
    ** pins this mapping. The exclusive sweep owner cannot enter an ordinary
    ** SMR reader, so gc2_mem_registered_ticketed() must accept its
    ** non-transferable reclaim-TLS certificate. FREEING is therefore valid
    ** here, unlike an ordinary pre-dispatch validator which has not established
    ** destructor ownership. */
    if (!tg || !lj_tg_flags_test_acq(tg, TGF_HUGETAB) ||
	lj_arena_hugetab_lookup(&tg->huge, base, &hi) != 1 ||
	size > hi.size)
      return 0;
  } else if (!gc2_size_fits_mem(g, base, size)) {
    return 0;
  }
  *basep = base;
  *sizep = size;
  return 1;
}

static int gc2_deferred_body_pending(global_State *g, GCobj *o)
{
  TGState *tg = G2TG(g);
  GCArena *a;
  uint32_t cell;
  if (!o || !tg || !lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL) ||
      g->allocf != lj_arena_allocf)
    return 0;
  a = lj_arena_of(o);
  if (lj_arena_ishuge(a) || !(a->hdr.flags & LJ_AF_TRAVERSABLE))
    return 0;
  cell = lj_arena_cellof(o);
  return cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS &&
	 lj_arena_bm_get(a->block, cell);
}

static int gc_unlink_root_obj_mode(global_State *g, GCobj *dead,
				   uint32_t limit, int terminal)
{
  GCRef *p;
  GCobj *o;
  GCobj *tortoise = NULL;
  GCRootStateRef rootstate;
  void *known_arena = NULL;
  uint32_t n = 0;
  uint64_t power = 1, lam = 0;
  int claim, result = LJ_GC_ROOT_UNLINK_UNPROVEN;
  if (!g || !dead)
    return LJ_GC_ROOT_UNLINK_UNPROVEN;
  /* A terminal walk is finite after threading shutdown, but GC workers may
  ** still be draining existing work while finalizers are separated. Take one
  ** try-only reader for the whole walk: an exclusive reclaimer makes this
  ** attempt defer instead of blocking, while admission pins every acquired
  ** root link until the predecessor CAS has resolved. */
  if (terminal && !lj_gc2_smr_read_try(g))
    return LJ_GC_ROOT_UNLINK_UNPROVEN;
  /* Claim before draining publisher stacks. A pending publisher only changes
  ** LINKING to MEMBER after its post-CAS hint, so either this observes LINKING
  ** and defers or UNLINKING excludes a concurrent publication of dead. */
  claim = gc_root_unlink_claim(g, dead, &rootstate);
  if (claim != LJ_GC_ROOT_UNLINKED) {
    result = claim;
    goto done;
  }
  (void)lj_gc_flush_root_pending(g);
  (void)lj_gc_repair_root_spine(g);
  p = lj_gc_root_ref(g);
  while ((o = gcref_acq(*p)) != NULL && (terminal || n < limit)) {
    GCobj *next;
    n++;
    if (LJ_UNLIKELY(terminal ?
	!gc_root_link_valid_held(g, o, &known_arena) :
	!gc_root_link_valid(g, o))) {
      /* Never interpret an inadmissible object's gcw. In particular, a stale
      ** ownership entry can have been reused as an interned string, where this
      ** word is the tagged string-hash successor. Sever only the acquired
      ** incoming edge and report that target absence could not be proved. */
      gc_root_unlink_restore(&rootstate);
      if (!gc_ref_cas_obj(p, o, NULL))
	goto done;
      (void)lj_gc_repair_root_spine(g);
      goto done;
    }
    next = lj_obj_gcw_acq(o);
    if (o == dead) {
      /* A valid self-link can only be damaged ownership metadata. Removing the
      ** exact incoming edge is sufficient; never publish the same pointer back
      ** through p and call that a successful splice. */
	if (gc_ref_cas_obj(p, o, next == o ? NULL : next)) {
	  int committed = gc_root_unlink_commit(g, &rootstate);
	  lj_assertG(committed, "root membership unlink commit lost");
	  UNUSED(committed);
	  result = LJ_GC_ROOT_UNLINKED;
	  goto done;
	}
      gc_root_unlink_restore(&rootstate);
      goto done;
    }
    if (LJ_UNLIKELY(next == o)) {
      gc_root_unlink_restore(&rootstate);
      (void)gc_ref_cas_obj(p, o, NULL);
      goto done;
    }
    if (terminal) {
      /* The repair epoch is only a publication hint: a prior exceptional
      ** repair can have cached the epoch after a validation or CAS loss. Do
      ** not let a surviving multi-node cycle turn this intentionally
      ** unbounded terminal walk into a shutdown livelock. Brent detection is
      ** allocation-free and reads only links admitted under our SMR scope. */
      if (!tortoise)
	tortoise = o;
      if (next) {
	lam++;
	if (LJ_UNLIKELY(next == tortoise)) {
	  gc_root_unlink_restore(&rootstate);
	  (void)gc_root_chain_break_cycle_held(g, lj_gc_root_acq(g));
	  goto done;
	}
	if (lam == power) {
	  tortoise = next;
	  power = power > ~(uint64_t)0 / 2u ? ~(uint64_t)0 : power << 1;
	  lam = 0;
	}
      }
    }
    p = lj_obj_gcwref(o);
  }
  if (o == NULL) {
    int committed = gc_root_unlink_commit(g, &rootstate);
    lj_assertG(committed, "absent root membership commit lost");
    UNUSED(committed);
    result = LJ_GC_ROOT_UNLINK_ABSENT;
    goto done;
  }
  gc_root_unlink_restore(&rootstate);
done:
  if (terminal)
    lj_gc2_smr_read_leave(g);
  return result;
}

int lj_gc_unlink_root_obj(global_State *g, GCobj *dead)
{
  return gc_unlink_root_obj_mode(g, dead, LJ_GC2_ROOT_SCAN_LIMIT, 0);
}

int lj_gc_unlink_root_obj_terminal(global_State *g, GCobj *dead)
{
  return gc_unlink_root_obj_mode(g, dead, 0, 1);
}

#define GC2_ROOT_PRUNE_BATCH 256u

static void *gc2_sweep_obj_base(global_State *g, GCobj *o)
{
  UNUSED(g);
#if LJ_HASFFI
  if (o && o->gch.gct == (uint32_t)~LJ_TCDATA) {
    GCcdata *cd = gco2cd(o);
    if (cdataisv(cd))
      return memcdatav(cd);
  }
#endif
  return o;
}

static int gc2_sweep_obj_old_generation(global_State *g, GCobj *o)
{
  void *base = gc2_sweep_obj_base(g, o);
  GCArena *a;
  if (!base || !checkptrGC(base))
    return 0;
  a = lj_arena_of(base);
  if (!lj_arena_ishuge(a))
    return (lj_arena_flags_acq(a) &
	    (LJ_AF_NEEDSWEEP|LJ_AF_QUARANTINE)) != 0;
  {
    uint32_t owner_tid = lj_arena_owner_acq(a);
    TGState *tg = lj_tg_find_owner(g, owner_tid);
    LJHugeInfo hi;
    return tg && lj_tg_flags_test_acq(tg, TGF_HUGETAB) &&
	   lj_arena_hugetab_lookup(&tg->huge, base, &hi) == 1 &&
	   (hi.flags & (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_TRAVERSABLE)) ==
	     (LJ_HUGEF_SWEEP_OLD|LJ_HUGEF_TRAVERSABLE);
  }
}

static int gc2_sweep_info_old_generation(global_State *g, GCobj *o,
					  const LJGC2QueuedInfo *info)
{
  GCArena *a = info ? (GCArena *)info->arena : NULL;
  if (a && !lj_arena_ishuge(a))
    return (lj_arena_flags_acq(a) &
	    (LJ_AF_NEEDSWEEP|LJ_AF_QUARANTINE)) != 0;
  return gc2_sweep_obj_old_generation(g, o);
}

static void gc2_sweep_mt_exclusive_leave(global_State *g)
{
  mt_gc_exclusive_rel(g, 0);
  mt_gc_exclusive_futex_wake(g, INT_MAX);
}

static int gc2_sweep_mt_exclusive_try(global_State *g)
{
  uint32_t expect = 0;
  TGState *self = lj_thr_get_tg();
  if (!g || self != g->main_tg || mt_active_acq(g) != 0 ||
      mt_live_acq(g) != 0 || mt_entering_acq(g) != 0 ||
      gc2_n_workers_acq(g) != 0 || lj_tg_any_jit_active(g) ||
      !mt_gc_exclusive_cas(g, &expect, 1))
    return 0;
  /* An entrant which published before the CAS is visible here. An entrant
  ** which starts afterward observes the gate and cannot enter the VM until the
  ** bounded root-prune pass releases it. */
  if (mt_active_acq(g) == 0 && mt_live_acq(g) == 0 &&
      mt_entering_acq(g) == 0 && gc2_n_workers_acq(g) == 0 &&
      !lj_tg_any_jit_active(g) && lj_thr_get_tg() == g->main_tg)
    return 1;
  gc2_sweep_mt_exclusive_leave(g);
  return 0;
}

static int gc2_sweep_detached_small(global_State *g, GCArena *a,
				    uint32_t cell, int marked);

static int gc2_sweep_exclusive_leaf_claim(global_State *g, GCobj *o,
					    const LJGC2QueuedInfo *info)
{
  GCArena *a = info ? (GCArena *)info->arena : NULL;
  uint32_t gct = info ? info->gct : 0;
  int claimed;
  if (!g || mt_gc_exclusive_acq(g) == 0 || !a ||
      info->base != (void *)o || lj_arena_ishuge(a) ||
      mt_active_or_entering_acq(g) || gc2_n_workers_acq(g) != 0 ||
      G2TG(g) != g->main_tg || lj_tg_any_jit_active(g) || info->marked ||
      !lj_arena_gc2_reclaim_clear_acq(a, info->start) ||
      (gct != (uint32_t)~LJ_TFUNC &&
	(gct != (uint32_t)~LJ_TUPVAL || !gco2uv(o)->closed)) ||
      lj_arena_sweep_state_acq(a, info->start) != LJ_ARENA_SWEEP_WHITE)
    return 0;
  claimed = lj_arena_lifetime_state_cas(a, info->start,
	LJ_ARENA_LIFETIME_LIVE, LJ_ARENA_LIFETIME_DESTRUCT);
  if (claimed && !lj_arena_gc2_reclaim_clear_acq(a, info->start)) {
    int restored = lj_arena_lifetime_state_cas(a, info->start,
	LJ_ARENA_LIFETIME_DESTRUCT, LJ_ARENA_LIFETIME_LIVE);
    lj_assertG(restored, "exclusive leaf token rollback lost ownership");
    UNUSED(restored);
    return 0;
  }
  return claimed;
}

static void gc2_sweep_exclusive_leaf_commit(global_State *g, GCobj *o,
					      const LJGC2QueuedInfo *info,
					      GCRootStateRef *rootstate)
{
  GCArena *a = (GCArena *)info->arena;
  int rootok, sweepok, lifeok, detached;
  if (!lj_arena_gc2_reclaim_clear_acq(a, info->start)) {
    lifeok = lj_arena_lifetime_state_cas(a, info->start,
	LJ_ARENA_LIFETIME_DESTRUCT, LJ_ARENA_LIFETIME_LIVE);
    detached = lifeok &&
	gc2_sweep_detached_small(g, a, info->start, 1);
    rootok = detached && gc_root_unlink_commit(g, rootstate);
    /* The incoming edge was already spliced. Convert this late global
    ** no-reclaim observation into the ordinary post-grace reanchor ticket
    ** instead of freeing or attempting to reconstruct the old predecessor. */
    lj_assertG(lifeok && detached && rootok,
	"exclusive leaf descriptor fallback lost ownership");
    if (LJ_UNLIKELY(!lifeok || !detached || !rootok))
      abort();
    return;
  }
  rootok = gc_root_unlink_commit(g, rootstate);
  sweepok = lj_arena_sweep_state_cas(a, info->start,
	LJ_ARENA_SWEEP_WHITE, LJ_ARENA_SWEEP_FREEING);
  lifeok = sweepok && lj_arena_lifetime_state_cas(a, info->start,
	LJ_ARENA_LIFETIME_DESTRUCT, LJ_ARENA_LIFETIME_FREE);
  lj_assertG(rootok && sweepok && lifeok,
	     "exclusive leaf sweep commit lost exact ownership");
  if (LJ_UNLIKELY(!rootok || !sweepok || !lifeok))
    abort();
  if (info->gct == (uint32_t)~LJ_TFUNC)
    lj_func_free(g, gco2func(o));
  else
    lj_func_freeuv(g, gco2uv(o));
}

static int gc2_sweep_exclusive_pair(global_State *g, GCRef *incoming,
	GCobj *fno, const LJGC2QueuedInfo *fninfo, void **known_arenap)
{
  LJGC2QueuedInfo uvinfo;
  GCArena *a = fninfo ? (GCArena *)fninfo->arena : NULL;
  GCfunc *fn;
  GCobj *uvo, *successor;
  uint32_t fncell, uvcell;
  int rootok, sweepok, lifeok;
  if (!g || mt_gc_exclusive_acq(g) == 0 || !incoming || !fno ||
      !fninfo || !a ||
      fninfo->base != (void *)fno || fninfo->gct != (uint32_t)~LJ_TFUNC ||
      fninfo->marked || lj_arena_ishuge(a) ||
      mt_active_or_entering_acq(g) || gc2_n_workers_acq(g) != 0 ||
      G2TG(g) != g->main_tg || lj_tg_any_jit_active(g) ||
      !lj_arena_gc2_reclaim_clear_acq(a, fninfo->start))
    return 0;
  fn = gco2func(fno);
  if (!isluafunc(fn) || lj_funcL_nupvalues(&fn->l) != 1)
    return 0;
  uvo = lj_obj_gcw_acq(fno);
  if (!uvo || uvo == fno || func_uvptr_acq(&fn->l, 0) != uvo ||
      lj_arena_of(uvo) != a || lj_arena_cellof(uvo) < LJ_AFIRST_CELL ||
      !lj_arena_gc2_reclaim_clear_acq(a, lj_arena_cellof(uvo)) ||
      !lj_gc2_obj_queued_info_held(g, uvo, a, &uvinfo))
    return 0;
  if (uvinfo.arena != a || uvinfo.base != (void *)uvo || uvinfo.marked ||
      uvinfo.gct != (uint32_t)~LJ_TUPVAL ||
      !lj_arena_gc2_reclaim_clear_acq(a, uvinfo.start) ||
      !gco2uv(uvo)->closed ||
      (lj_obj_gcflags(fno) & (LJ_GC_FIXED|LJ_GC_SFIXED)) ||
      (lj_obj_gcflags(uvo) & (LJ_GC_FIXED|LJ_GC_SFIXED)) ||
      !gc2_sweep_info_old_generation(g, uvo, &uvinfo) ||
      !gc2_valid_freeable_queued(g, fno, fninfo) ||
      !gc2_valid_freeable_queued(g, uvo, &uvinfo))
    return 0;
  fncell = fninfo->start;
  uvcell = uvinfo.start;
  if (lj_arena_sweep_state_acq(a, fncell) != LJ_ARENA_SWEEP_WHITE ||
      lj_arena_sweep_state_acq(a, uvcell) != LJ_ARENA_SWEEP_WHITE ||
      !lj_arena_root_state_cas_pair(a, fncell, uvcell,
	LJ_ARENA_ROOT_MEMBER, LJ_ARENA_ROOT_UNLINKING))
    return 0;
  gc_test_root_state(g, fno, LJ_GC_ROOT_STATE_TEST_UNLINKING);
  gc_test_root_state(g, uvo, LJ_GC_ROOT_STATE_TEST_UNLINKING);
  if (mt_gc_exclusive_acq(g) == 0 || mt_active_or_entering_acq(g) ||
      gc2_n_workers_acq(g) != 0 ||
      lj_tg_any_jit_active(g) || lj_arena_bm_get(a->mark, fncell) ||
      lj_arena_bm_get(a->mark, uvcell) ||
      !lj_arena_lifetime_state_cas_pair(a, fncell, uvcell,
	LJ_ARENA_LIFETIME_LIVE, LJ_ARENA_LIFETIME_DESTRUCT)) {
    rootok = lj_arena_root_state_cas_pair(a, fncell, uvcell,
	LJ_ARENA_ROOT_UNLINKING, LJ_ARENA_ROOT_MEMBER);
    lj_assertG(rootok, "exclusive pair root rollback lost ownership");
    UNUSED(rootok);
    return 0;
  }
  if (!lj_arena_gc2_reclaim_clear_acq(a, fncell) ||
      !lj_arena_gc2_reclaim_clear_acq(a, uvcell)) {
    lifeok = lj_arena_lifetime_state_cas_pair(a, fncell, uvcell,
	LJ_ARENA_LIFETIME_DESTRUCT, LJ_ARENA_LIFETIME_LIVE);
    rootok = lj_arena_root_state_cas_pair(a, fncell, uvcell,
	LJ_ARENA_ROOT_UNLINKING, LJ_ARENA_ROOT_MEMBER);
    lj_assertG(lifeok && rootok,
	       "exclusive pair token rollback lost ownership");
    UNUSED(lifeok); UNUSED(rootok);
    return 0;
  }
  successor = lj_obj_gcw_acq(uvo);
  if (successor == fno || successor == uvo ||
      !gc_ref_cas_obj(incoming, fno, successor)) {
    lifeok = lj_arena_lifetime_state_cas_pair(a, fncell, uvcell,
	LJ_ARENA_LIFETIME_DESTRUCT, LJ_ARENA_LIFETIME_LIVE);
    rootok = lj_arena_root_state_cas_pair(a, fncell, uvcell,
	LJ_ARENA_ROOT_UNLINKING, LJ_ARENA_ROOT_MEMBER);
    lj_assertG(lifeok && rootok,
	       "exclusive pair splice rollback lost ownership");
    UNUSED(lifeok); UNUSED(rootok);
    return 0;
  }

  if (!lj_arena_gc2_reclaim_clear_acq(a, fncell) ||
      !lj_arena_gc2_reclaim_clear_acq(a, uvcell)) {
    lifeok = lj_arena_lifetime_state_cas_pair(a, fncell, uvcell,
	LJ_ARENA_LIFETIME_DESTRUCT, LJ_ARENA_LIFETIME_LIVE);
    if (lifeok) {
      (void)gc2_sweep_detached_small(g, a, fncell, 1);
      (void)gc2_sweep_detached_small(g, a, uvcell, 1);
    }
    rootok = lifeok && lj_arena_root_state_cas_pair(a, fncell, uvcell,
	LJ_ARENA_ROOT_UNLINKING, LJ_ARENA_ROOT_NONE);
    if (rootok)
      lj_arena_recovery_complete_wake(a);
    lj_assertG(lifeok && rootok,
	"exclusive pair descriptor fallback lost ownership");
    if (LJ_UNLIKELY(!lifeok || !rootok))
      abort();
    if (known_arenap)
      *known_arenap = a;
    return 1;
  }

  rootok = lj_arena_root_state_cas_pair(a, fncell, uvcell,
	LJ_ARENA_ROOT_UNLINKING, LJ_ARENA_ROOT_NONE);
  if (rootok)
    lj_arena_recovery_complete_wake(a);
  sweepok = rootok && lj_arena_sweep_state_cas_pair(a, fncell, uvcell,
	LJ_ARENA_SWEEP_WHITE, LJ_ARENA_SWEEP_FREEING);
  lifeok = sweepok && lj_arena_lifetime_state_cas_pair(a, fncell, uvcell,
	LJ_ARENA_LIFETIME_DESTRUCT, LJ_ARENA_LIFETIME_FREE);
  lj_assertG(rootok && sweepok && lifeok,
	     "exclusive closure pair sweep commit lost exact ownership");
  if (LJ_UNLIKELY(!rootok || !sweepok || !lifeok))
    abort();
  lj_func_free(g, fn);
  lj_func_freeuv(g, gco2uv(uvo));
  if (known_arenap)
    *known_arenap = a;
  return 1;
}

/* Classify one exact small old-generation allocation after its ownership-spine
** link has been detached. LIVE means it must be reanchored after the grace;
** RETIRED means its destructor is pending. WHITE remains reserved for raw or
** fixed allocations which were never detached. */
static int gc2_sweep_detached_small(global_State *g, GCArena *a,
				    uint32_t cell, int marked)
{
  uint32_t state = lj_arena_sweep_state_acq(a, cell);
  if (marked) {
    for (;;) {
      if (state == LJ_ARENA_SWEEP_LIVE ||
	  state == LJ_ARENA_SWEEP_FREEING)
	break;
      if (lj_arena_sweep_state_cas(a, cell, state,
					   LJ_ARENA_SWEEP_LIVE)) {
	if (state == LJ_ARENA_SWEEP_RETIRED) {
	  uint32_t old = lj_arena_reclaim_deferred_sub(a, 1);
	  lj_assertG(old != 0, "detached-root rescue underflow");
	  UNUSED(old);
	}
	break;
      }
      state = lj_arena_sweep_state_acq(a, cell);
    }
  } else if (state == LJ_ARENA_SWEEP_WHITE) {
    uint32_t old;
    /* Reserve the counter before publishing RETIRED. A sweep-time publisher
    ** which wins RETIRED->LIVE can then decrement without racing an increment. */
    (void)lj_arena_reclaim_deferred_add(a, 1);
    if (!lj_arena_sweep_state_cas(a, cell, LJ_ARENA_SWEEP_WHITE,
					  LJ_ARENA_SWEEP_RETIRED)) {
      old = lj_arena_reclaim_deferred_sub(a, 1);
      lj_assertG(old != 0, "detached-root retire rollback underflow");
    } else if (lj_arena_bm_get(a->mark, cell) &&
	       lj_arena_sweep_state_cas(a, cell, LJ_ARENA_SWEEP_RETIRED,
					  LJ_ARENA_SWEEP_LIVE)) {
      old = lj_arena_reclaim_deferred_sub(a, 1);
      lj_assertG(old != 0, "detached-root overlap rescue underflow");
    }
    UNUSED(old);
  }
  gc2_sweep_grace_needed_rel(g, 1);
  return 1;
}

static int gc2_sweep_detached_obj(global_State *g, GCobj *o, int marked)
{
  void *base = gc2_sweep_obj_base(g, o);
  GCArena *a = lj_arena_of(base);
  if (!lj_arena_ishuge(a)) {
    uint32_t cell = lj_arena_cellof(base);
    if (cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS)
      return gc2_sweep_detached_small(g, a, cell, marked);
    return 0;
  }
  {
    uint32_t owner_tid = lj_arena_owner_acq(a);
    TGState *tg = lj_tg_find_owner(g, owner_tid);
    int ticketed;
    if (!tg || !lj_tg_flags_test_acq(tg, TGF_HUGETAB))
      return 0;
    /* One huge mapping contains one allocation. Publish an explicit metadata
    ** ticket for both dead and already-marked detached roots: RETIRED is the
    ** destructor claim, while MARK|TICKET is the post-grace reanchor claim.
    ** A concurrent marker may win either side of this call without making the
    ** exact variable-offset header undiscoverable. */
    {
      LJHugeInfo hi;
      ticketed = lj_arena_hugetab_retire(&tg->huge, base, o,
						lj_gc2_retire_epoch(g), &hi);
      /* A marker that meets SWEEP_OLD|BUSY cannot read the header or wait for
      ** the retire owner. It publishes MARK_INTENT instead; return 2 makes this
      ** unique owner discharge exactly one semantic traversal after TICKET has
      ** made retire_obj and the mapping readable again. */
      if (ticketed == 2)
	(void)lj_gc2_trace_sweep_root(g, o);
    }
    lj_assertG(ticketed, "detached huge root lost ownership ticket");
    if (!ticketed)
      return 0;  /* Retain the mapped body; never invent a destructor owner. */
    gc2_sweep_grace_needed_rel(g, 1);
    UNUSED(marked);
    return 1;
  }
}

uint32_t lj_gc_sweep_gc2_unmarked(global_State *g)
{
  GCRef *p;
  GCobj *o, *maino, *vmo;
  lua_State *mainL, *vmL;
  void *known_arena = NULL;
  uint32_t seen = 0, unlinked = 0;
  int mt_exclusive;
  if (!g || gc2_phase_acq(g) != LJ_GC2_SWEEP ||
      gc2_sweep_root_done_acq(g))
    return 0;
  mt_exclusive = gc2_sweep_mt_exclusive_try(g);
  /* Keep registry membership stable across this bounded spine segment. Nested
  ** validators use the thread-local SMR depth and therefore avoid one global
  ** reader acquire/release pair per ownership entry. The writer remains
  ** nonwaiting: it observes this reader and retries after the bounded batch. */
  if (!lj_gc2_smr_read_try(g)) {
    if (mt_exclusive)
      gc2_sweep_mt_exclusive_leave(g);
    return 0;
  }
  p = gc2_sweep_root_cursor_acq(g);
  if (!p)
    p = lj_gc_root_ref(g);
  mainL = mainthread_acq(g);
  vmL = vmthread_acq(g);
  maino = mainL ? obj2gco(mainL) : NULL;
  vmo = vmL ? obj2gco(vmL) : NULL;
  while (seen < GC2_ROOT_PRUNE_BATCH && (o = gcref_acq(*p)) != NULL) {
    LJGC2QueuedInfo info;
    uint32_t gct;
    int marked;
    seen++;
    /* Permanent threads are state-lifetime roots and do not need allocator
    ** identity. Every ordinary spine member gets one non-semantic structural
    ** snapshot which is reused for mark, generation and unlink decisions. */
    if (o == maino || o == vmo) {
      p = lj_obj_gcwref(o);
      continue;
    }
    if (LJ_UNLIKELY(!lj_gc2_obj_queued_info_held(
		      g, o, known_arena, &info) ||
		    info.gct == (uint32_t)~LJ_TSTR)) {
      /* No successor can be read from an inadmissible ownership entry: a reused
      ** string address, for example, uses the same word as a hash-chain link.
      ** Sever the exact incoming edge instead of restarting forever at the
      ** same stale node or following a foreign intrusive list. Lost ownership
      ** metadata is fail-closed (the arena stays retained); semantic/pending
      ** roots republish valid objects independently. */
      if (!gc_ref_cas_obj(p, o, NULL))
	continue;
      (void)lj_gc_repair_root_spine(g);
      unlinked++;
      break;
    }
    known_arena = info.start != 0 ? info.arena : NULL;
    gct = info.gct;
    gc2_preserve_root_spine_body(g, o);
    /* mainthread->gcw is also the lock-free insertion point for userdata and
    ** secondary lua_State ownership. Detaching/reanchoring the permanent
    ** thread while an after-main publisher CASes that word is a two-location
    ** race: generic lj_gc_linkobj() can overwrite the newly appended chain.
    ** Neither permanent thread is runtime-collectable, so retain both as
    ** stable spine anchors and prune only their successors. */
    if (!gc2_sweep_info_old_generation(g, o, &info)) {
      p = lj_obj_gcwref(o);  /* Post-reset allocation: never this sweep. */
      continue;
    }
    marked = (int)info.marked;
    if (LJ_UNLIKELY(gc2_cdata_finalizer_pending(o)) ||
	LJ_UNLIKELY(gc2_udata_finalizer_pending(o)) ||
	(lj_obj_gcflags(o) & (LJ_GC_FIXED|LJ_GC_SFIXED))) {
      /* FINREG dispatch owns both finalizer classes until it requeues them and
      ** only then clears the registration bit. Detaching a registered userdata
      ** here would create a second LIVE reanchor ticket: dispatch could insert
      ** it after main while the post-grace ticket inserts the same intrusive
      ** object globally, forming a cycle with two incoming ownership edges. */
      (void)lj_gc2_markmem(g, info.base);
      p = lj_obj_gcwref(o);
      continue;
    }
    if (mt_exclusive && seen < GC2_ROOT_PRUNE_BATCH && marked == 0 &&
	gc2_sweep_exclusive_pair(g, p, o, &info, &known_arena)) {
      seen++;
      unlinked += 2;
      continue;
    }
    if (gct == 0 || gc2_valid_freeable_queued(g, o, &info)) {
      GCRootStateRef rootstate;
      int exclusive_leaf;
      int rootclaim = gc_root_unlink_claim_queued(g, o, &info, &rootstate);
      if (rootclaim != LJ_GC_ROOT_UNLINKED) {
	/* NONE is an unconverted direct-VM publication; LINKING/UNLINKING is an
	** in-flight C publisher/remover. Neither is proof that this incoming edge
	** belongs to us. Retain the allocation and its graph for this generation
	** without changing gcw. */
	(void)lj_gc2_markmem(g, info.base);
	p = lj_obj_gcwref(o);
	continue;
      }
#if LJ_HASJIT
      if (marked == 0 && gct == (uint32_t)~LJ_TTRACE &&
	  !lj_trace_retire_gc_claim(g, gco2trace(o))) {
	/* The nonwaiting recorder-token attempt left trace/root/state unchanged. */
	gc_root_unlink_restore(&rootstate);
	break;
      }
#endif
      exclusive_leaf = mt_exclusive ?
	gc2_sweep_exclusive_leaf_claim(g, o, &info) : 0;
      if (!gc_chain_splice(p, o)) {
	/* Do not monopolize UNLINKING against predecessor churn. A later bounded
	** root pass can acquire a fresh claim and retry the exact edge. */
	if (exclusive_leaf) {
	  int restored = lj_arena_lifetime_state_cas(
	    (GCArena *)info.arena, info.start,
	    LJ_ARENA_LIFETIME_DESTRUCT, LJ_ARENA_LIFETIME_LIVE);
	  lj_assertG(restored, "exclusive leaf sweep rollback lost lifetime");
	  UNUSED(restored);
	}
	gc_root_unlink_restore(&rootstate);
	break;
      }
      if (exclusive_leaf) {
	gc2_sweep_exclusive_leaf_commit(g, o, &info, &rootstate);
	unlinked++;
	continue;
      }
      if (LJ_UNLIKELY(!gc2_sweep_detached_obj(g, o, marked > 0))) {
	/* The object is no longer reachable from the spine, so restoring MEMBER
	** would lie. Keep UNLINKING as a fail-closed arena-reuse veto. */
	lj_assertG(0, "detached root lost sweep classification");
	break;
      }
      {
	int committed = gc_root_unlink_commit(g, &rootstate);
	lj_assertG(committed, "sweep root membership commit lost");
	UNUSED(committed);
      }
      unlinked++;
      continue;
    }
    p = lj_obj_gcwref(o);
  }
  gc2_sweep_root_cursor_rel(g, p);
  if (gcref_acq(*p) == NULL) {
    uint32_t flushed = lj_gc_flush_root_pending(g);
    (void)lj_gc_repair_root_spine(g);
    if (flushed != 0 || lj_gcroot_pending_hint_acq(g) != 0) {
      gc2_sweep_root_cursor_rel(g, lj_gc_root_ref(g));
    } else {
      gc2_sweep_root_done_rel(g, 1);
      gc2_sweep_root_cursor_rel(g, NULL);
    }
  }
  lj_gc2_smr_read_leave(g);
  if (mt_exclusive)
    gc2_sweep_mt_exclusive_leave(g);
  return unlinked;
}

static uint32_t gc2_sweep_alloc_end(const GCArena *a, uint32_t start)
{
  uint32_t i = start + 1u;
  while (i < LJ_ARENA_CELLS &&
	 !lj_arena_bm_get(a->block, i) && !lj_arena_bm_get(a->mark, i))
    i++;
  return i;
}

static LJ_AINLINE int gc2_sweep_dtor_kind_supported(uint32_t kind)
{
  return kind == LJ_ARENA_DTOR_LFUNC1 ||
	 kind == LJ_ARENA_DTOR_CLOSED_UV ||
	 kind == LJ_ARENA_DTOR_LFUNC0;
}

/* Collapse one packed 32-cell sweep word to one bit per non-WHITE lane. */
static LJ_AINLINE uint32_t gc2_sweep_nonwhite32(uint64_t sweep)
{
  uint64_t bits = (sweep | (sweep >> 1)) &
    UINT64_C(0x5555555555555555);
  bits = (bits | (bits >> 1)) & UINT64_C(0x3333333333333333);
  bits = (bits | (bits >> 2)) & UINT64_C(0x0f0f0f0f0f0f0f0f);
  bits = (bits | (bits >> 4)) & UINT64_C(0x00ff00ff00ff00ff);
  bits = (bits | (bits >> 8)) & UINT64_C(0x0000ffff0000ffff);
  bits = (bits | (bits >> 16)) & UINT64_C(0x00000000ffffffff);
  return (uint32_t)bits;
}

/* Exactly one of the three supported one-hot planes, with plane 3 clear. */
static LJ_AINLINE uint64_t gc2_sweep_supported_dtor64(uint64_t p0,
	uint64_t p1, uint64_t p2, uint64_t p3)
{
  uint64_t multi = (p0 & p1) | (p0 & p2) | (p1 & p2);
  return (p0 | p1 | p2) & ~multi & ~p3;
}

static LJ_AINLINE void gc2_sweep_partition64(uint64_t block, uint64_t mark,
	uint64_t sweep0, uint64_t sweep1, uint64_t p0, uint64_t p1,
	uint64_t p2, uint64_t p3, uint64_t valid, uint64_t *pinp,
	uint64_t *candidatep)
{
  uint64_t nonwhite = (uint64_t)gc2_sweep_nonwhite32(sweep0) |
	((uint64_t)gc2_sweep_nonwhite32(sweep1) << 32);
  uint64_t todo = block & ~mark & ~nonwhite & valid;
  uint64_t supported = gc2_sweep_supported_dtor64(p0, p1, p2, p3);
  *pinp = todo & ~supported;
  *candidatep = todo & supported;
}

static LJ_AINLINE uint64_t gc2_sweep_bulk_pin64(uint64_t *mark,
	uint64_t pin)
{
  return la_or64_rlx(mark, pin);
}

#ifdef LJ_GC2_TEST_HELPERS
static LJGcSelectedCasHook gc2_test_selected_cas_hook;
#endif

/* Atomically replace all selected packed lanes or none. A CAS loss caused by
** an unrelated lane rebuilds from the returned word and preserves that lane;
** a selected-lane disagreement returns without modifying any selected lane. */
static LJ_AINLINE int gc2_sweep_selected_cas(uint64_t *word,
	uint64_t lane_lsb, uint32_t lane_bits, uint32_t from, uint32_t to)
{
  uint32_t lane_value_mask;
  uint64_t lane_lsb_mask, selected, expected, replacement, old;
  if (!word || lane_lsb == 0)
    return 0;
  if (lane_bits == 4u)
    lane_lsb_mask = UINT64_C(0x1111111111111111);
  else if (lane_bits == 2u)
    lane_lsb_mask = UINT64_C(0x5555555555555555);
  else
    return 0;
  if ((lane_lsb & ~lane_lsb_mask) != 0)
    return 0;
  lane_value_mask = ((uint32_t)1u << lane_bits) - 1u;
  if (from > lane_value_mask || to > lane_value_mask)
    return 0;
  selected = lane_lsb * lane_value_mask;
  expected = lane_lsb * from;
  replacement = lane_lsb * to;
  old = la_load64_acq(word);
#ifdef LJ_GC2_TEST_HELPERS
  if (gc2_test_selected_cas_hook)
    gc2_test_selected_cas_hook(word);
#endif
  for (;;) {
    uint64_t next;
    if ((old & selected) != expected)
      return 0;
    next = (old & ~selected) | replacement;
    if (la_cas64(word, &old, next, LA_ACQ_REL, LA_ACQ))
      return 1;
  }
}

static LJ_AINLINE int gc2_sweep_lifetime_selected_cas(uint64_t *word,
	uint64_t lane_lsb, uint32_t from, uint32_t to)
{
  return gc2_sweep_selected_cas(word, lane_lsb, 4u, from, to);
}

static LJ_AINLINE int gc2_sweep_sweep_selected_cas(uint64_t *word,
	uint64_t lane_lsb, uint32_t from, uint32_t to)
{
  return gc2_sweep_selected_cas(word, lane_lsb, 2u, from, to);
}

#ifdef LJ_GC2_TEST_HELPERS
static LJGcSweepBatchHook gc2_test_sweep_batch_hook;
static uint32_t gc2_test_sweep_batch_commit_count;
static uint32_t gc2_test_sweep_batch_object_count;

void lj_gc_test_set_selected_cas_hook(LJGcSelectedCasHook hook)
{
  gc2_test_selected_cas_hook = hook;
}

int lj_gc_test_lifetime_selected_cas(uint64_t *word, uint64_t lane_lsb,
	uint32_t from, uint32_t to)
{
  return gc2_sweep_lifetime_selected_cas(word, lane_lsb, from, to);
}

int lj_gc_test_sweep_selected_cas(uint64_t *word, uint64_t lane_lsb,
	uint32_t from, uint32_t to)
{
  return gc2_sweep_sweep_selected_cas(word, lane_lsb, from, to);
}

void lj_gc_test_set_sweep_batch_hook(LJGcSweepBatchHook hook)
{
  gc2_test_sweep_batch_hook = hook;
}

void lj_gc_test_sweep_batch_stats_reset(void)
{
  la_store32_rel(&gc2_test_sweep_batch_commit_count, 0);
  la_store32_rel(&gc2_test_sweep_batch_object_count, 0);
}

uint32_t lj_gc_test_sweep_batch_commits(void)
{
  return la_load32_acq(&gc2_test_sweep_batch_commit_count);
}

uint32_t lj_gc_test_sweep_batch_objects(void)
{
  return la_load32_acq(&gc2_test_sweep_batch_object_count);
}

uint32_t lj_gc_test_sweep_nonwhite32(uint64_t sweep)
{
  return gc2_sweep_nonwhite32(sweep);
}

uint64_t lj_gc_test_sweep_supported_dtor64(uint64_t p0, uint64_t p1,
	uint64_t p2, uint64_t p3)
{
  return gc2_sweep_supported_dtor64(p0, p1, p2, p3);
}

void lj_gc_test_sweep_partition64(uint64_t block, uint64_t mark,
	uint64_t sweep0, uint64_t sweep1, uint64_t p0, uint64_t p1,
	uint64_t p2, uint64_t p3, uint64_t valid, uint64_t *pinp,
	uint64_t *candidatep)
{
  gc2_sweep_partition64(block, mark, sweep0, sweep1, p0, p1, p2, p3,
	valid, pinp, candidatep);
}

uint64_t lj_gc_test_sweep_bulk_pin64(uint64_t *mark, uint64_t pin)
{
  return gc2_sweep_bulk_pin64(mark, pin);
}
#endif

/* Validate a sidecar-selected destructor without deriving its identity from
** mutable body bytes. The immutable arena kind is the authority; the header
** and exact block extent only have to agree before the ordinary type-specific
** destructor may run. */
static GCobj *gc2_sweep_dtor_obj(global_State *g, GCArena *a,
				  uint32_t cell, uint32_t end,
				  uint32_t kind, int vector_reclaim_owned)
{
  GCobj *o;
  GCSize size;
  if (!g || !a || cell < LJ_AFIRST_CELL || end <= cell ||
      end > LJ_ARENA_CELLS || !lj_arena_ready_get(a, cell) ||
      lj_arena_root_state_acq(a, cell) != LJ_ARENA_ROOT_NONE)
    return NULL;
  o = (GCobj *)lj_arena_cellptr(a, cell);
  /* Sidecar identity must not override another allocation family or permanent
  ** semantic retention. The caller owns either the post-grace reclaim lease or
  ** the local pre-grace MT/SMR/arena-seal capability; disagreement is retained
  ** rather than dispatched through either body's mutable bytes. */
  if (lj_arena_cdata_get(a, cell) ||
      (lj_obj_gcflags(o) & (LJ_GC_FIXED|LJ_GC_SFIXED)))
    return NULL;
  if (kind == LJ_ARENA_DTOR_CLOSED_UV) {
    GCupval *uv;
    size = (GCSize)sizeof(GCupval);
    if (la_load8_acq(&o->gch.gct) != (uint8_t)~LJ_TUPVAL)
      return NULL;
    uv = gco2uv(o);
    if (!uv->closed || uvval(uv) != &uv->tv)
      return NULL;
  } else if (kind == LJ_ARENA_DTOR_LFUNC0 ||
	     kind == LJ_ARENA_DTOR_LFUNC1) {
    GCfunc *fn;
    MSize expected = kind == LJ_ARENA_DTOR_LFUNC1 ? 1u : 0u;
    size = (GCSize)sizeLfunc(expected);
    if (la_load8_acq(&o->gch.gct) != (uint8_t)~LJ_TFUNC)
      return NULL;
    fn = gco2func(o);
    if (!isluafunc(fn) || lj_funcL_nupvalues(&fn->l) != expected)
      return NULL;
  } else {
    return NULL;
  }
  if (lj_arena_ncells(size) != end - cell ||
      !gc2_valid_freeable_obj(g, o, vector_reclaim_owned))
    return NULL;
  return o;
}

static GCobj *gc2_sweep_cell_obj(global_State *g, GCArena *a,
				  uint32_t cell, uint32_t end)
{
  GCobj *o = (GCobj *)lj_arena_cellptr(a, cell);
  uint32_t dtor_kind = lj_arena_dtor_kind_acq(a, cell);
  if (dtor_kind != LJ_ARENA_DTOR_NONE)
    return gc2_sweep_dtor_obj(g, a, cell, end, dtor_kind, 1);
#if LJ_HASFFI
  if (lj_arena_ready_get(a, cell) && lj_arena_cdata_get(a, cell)) {
    char *base = (char *)o;
    size_t bytes = (size_t)(end - cell) << LJ_CELL_SHIFT;
    if (o->gch.gct == (uint32_t)~LJ_TCDATA &&
	!cdataisv(gco2cd(o))) {
      void *realbase = NULL;
      GCSize size = 0;
      if (lj_cdata_validate(g, gco2cd(o), &realbase, &size) &&
	  realbase == (void *)base && size <= bytes &&
	  lj_arena_ncells(size) == end - cell &&
	  cdata_size_tail_matches(gco2cd(o), (size_t)size))
	return o;
    }
    /* If the base is not an exact fixed cdata header, allocation-owned prefix
    ** offset identifies the variable/over-aligned interior header. */
    uint16_t offset = la_load16_acq((uint16_t *)(void *)base);
    if (offset >= sizeof(GCcdataVar) &&
	(size_t)offset + sizeof(GCcdata) <= bytes) {
      GCcdata *cd = (GCcdata *)(void *)(base + offset);
      void *realbase = NULL;
      GCSize size = 0;
      if (cd->gct == ~LJ_TCDATA && cdataisv(cd) &&
	  memcdatav(cd) == (void *)base &&
	  lj_cdata_validate(g, cd, &realbase, &size) &&
	  realbase == (void *)base && size <= bytes &&
	  lj_arena_ncells(size) == end - cell &&
	  cdata_size_tail_matches(cd, (size_t)size))
	return obj2gco(cd);
    }
  }
#else
  UNUSED(end);
#endif
  if (o->gch.gct == 0 || gc2_valid_freeable_obj(g, o, 1))
    return o;
  return NULL;
}

static LJ_AINLINE int gc2_sweep_pregrace_recorder_active(global_State *g)
{
#if LJ_HASJIT
  return g && lj_trace_state_load(G2J(g)) != LJ_TRACE_IDLE;
#else
  UNUSED(g);
  return 0;
#endif
}

/* The owner-progress caller has already established the complete SSB fixpoint.
** Once this call locally owns the MT gate, no VM or worker producer can reopen
** it. Recheck every cheaply observable publication immediately before each
** destructor transaction; the exact allocation lanes below remain the final
** authority if a non-VM terminal actor raced this bounded scan. */
static LJ_AINLINE int gc2_sweep_pregrace_work_clear(global_State *g)
{
  TGState *tg = g ? g->main_tg : NULL;
  return g && tg &&
	 gc2_recovery_items_acq(g) == 0 &&
	 gc2_recovery_huge_items_acq(g) == 0 &&
	 gc2_recovery_failed_acq(g) == 0 &&
	 gc2_assist_active_acq(g) == 0 &&
	 gc2_weak_drain_active_acq(g) == 0 &&
	 gc2_weak_write_active_acq(g) == 0 &&
	 gc2_ssb_consumer_active_acq(g) == 0 &&
	 gc2_ssb_head_acq(g) == NULL && gc2_ssb_drain_acq(g) == NULL &&
	 gc2_grey_top_acq(g) >= gc2_grey_bottom_acq(g) &&
	 gc2_thread_scan_needscan_pending_acq(g) == 0 &&
	 gc2_table_rescan_pending_acq(g) == 0 &&
	 gc2_marks_this_round_acq(g) == 0 &&
	 gc2_finalizer_tail_acq(g) == NULL &&
	 gc2_finalizer_mpsc_acq(g) == NULL &&
	 gc2_finalizer_active_acq(g) == 0 &&
	 lj_state_gcprep_pending_acq(g) == 0 &&
	 lj_gcroot_pending_hint_acq(g) == 0 &&
	 lj_tg_ssb_next_acq(tg) == lj_tg_ssb_base_acq(tg);
}

/* Establish the scan-wide capability. The caller samples this immediately
** before taking the exact arena seal and repeats it after clearing the sealed
** generation. The locally owned MT gate, worker token, SMR reader lease and
** seal then keep these identities and exclusions stable until release. Do not
** repeat this full certificate per allocation; dynamic work has its own test. */
static int gc2_sweep_pregrace_cap_ready(global_State *g, GCArena *a,
					 int mt_exclusive)
{
  TGState *main_tg = g ? g->main_tg : NULL;
  uint32_t flags;
  if (!mt_exclusive || !g || !a || !main_tg ||
	gc2_phase_acq(g) != LJ_GC2_SWEEP ||
	!gc2_sweep_bridge_ready_acq(g) ||
	gc2_sweep_root_scanned_acq(g) != 1 ||
	!gc2_sweep_root_done_acq(g) ||
	!lj_gc2_sweep_bridge_can_progress(g) ||
	lj_thr_get_tg() != main_tg || G2TG(g) != main_tg ||
	(lj_tg_flags_acq(main_tg) & (TGF_DEAD|TGF_ARENA_INTERNAL)) !=
	  TGF_ARENA_INTERNAL ||
	g->allocf != lj_arena_allocf || la_load32_acq(&g->allocf_arena) == 0 ||
	g->allocd != &main_tg->allocd ||
	mt_gc_exclusive_acq(g) == 0 || mt_active_acq(g) != 0 ||
	mt_live_acq(g) != 0 ||
	mt_entering_acq(g) != 0 || gc2_n_workers_acq(g) != 0 ||
	gc2_worker_active_acq(g) != 1 || gc2_smr_reclaiming_acq(g) != 0 ||
	gc2_jit_phase_gate_acq(g) != 0 || lj_tg_any_jit_active(g) ||
	gc2_sweep_pregrace_recorder_active(g))
    return 0;
  flags = lj_arena_flags_acq(a);
  if ((flags & (LJ_AF_TRAVERSABLE|LJ_AF_NEEDSWEEP)) !=
	(LJ_AF_TRAVERSABLE|LJ_AF_NEEDSWEEP) ||
      (flags & (LJ_AF_PREPSWEEP|LJ_AF_QUARANTINE|LJ_AF_RECLAIMED)) != 0 ||
      lj_arena_owner_acq(a) != lj_tg_tid_acq(main_tg))
    return 0;
  return 1;
}

/* Publications may still transiently contend with the held capability. Sample
** their semantic work before the exact remote generation: a publisher dirties
** that generation before making its work durable, so the final acquire cannot
** miss both halves. This must run after the admission fence and immediately
** before every terminal FREE claim. */
static LJ_AINLINE int gc2_sweep_pregrace_quiet(global_State *g, GCArena *a,
						int pregrace_owned)
{
  return pregrace_owned && g && a &&
	 gc2_smr_reclaiming_acq(g) == 0 &&
	 !lj_gc2_activation_reclaim_veto(g) &&
	 gc2_sweep_pregrace_work_clear(g) &&
	 lj_arena_remote_active_acq(a) ==
	   (LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_SEALED);
}

/* Exact per-allocation authority. `end` is the one allocation-boundary
** snapshot taken by the sealed scanner and already checked against the fixed
** kind's size. The seal and exact remote-generation checks protect that
** snapshot, so this hot predicate only reloads independently published lanes. */
static LJ_AINLINE int gc2_sweep_pregrace_obj_ready(GCArena *a,
						    uint32_t cell,
						    uint32_t end,
						    uint32_t kind,
						    uint32_t expected_lifetime)
{
  if (!a || cell < LJ_AFIRST_CELL || end <= cell ||
      end > LJ_ARENA_CELLS || !lj_arena_bm_get(a->block, cell) ||
      lj_arena_bm_get(a->mark, cell) ||
      lj_arena_sweep_state_acq(a, cell) != LJ_ARENA_SWEEP_WHITE ||
      lj_arena_dtor_kind_acq(a, cell) != kind ||
      !lj_arena_ready_get(a, cell) ||
      lj_arena_root_state_acq(a, cell) != LJ_ARENA_ROOT_NONE ||
      lj_arena_recovery_state_acq(a, cell) != LJ_ARENA_RECOVERY_IDLE ||
      !lj_arena_gc2_reclaim_clear_acq(a, cell) ||
      lj_arena_lifetime_state_acq(a, cell) != expected_lifetime ||
      lj_arena_late_get(a, cell))
    return 0;
  return 1;
}

static LJ_AINLINE GCSize gc2_sweep_pregrace_dtor_size(uint32_t kind)
{
  if (kind == LJ_ARENA_DTOR_LFUNC0)
    return (GCSize)sizeLfunc(0);
  if (kind == LJ_ARENA_DTOR_LFUNC1)
    return (GCSize)sizeLfunc(1);
  if (kind == LJ_ARENA_DTOR_CLOSED_UV)
    return (GCSize)sizeof(GCupval);
  return 0;
}

static int gc2_sweep_pregrace_terminal_owned(GCArena *a, uint32_t cell)
{
  return lj_arena_lifetime_state_acq(a, cell) ==
	   LJ_ARENA_LIFETIME_FREE &&
	 lj_arena_sweep_state_acq(a, cell) == LJ_ARENA_SWEEP_FREEING;
}

/* Release a tentative body claim without stealing recovery's cancellation.
** RESCUE owns the eventual restore and durable recovery publication. */
static void gc2_sweep_pregrace_claim_restore(global_State *g, GCArena *a,
					      uint32_t cell)
{
  uint32_t life;
  if (lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_DESTRUCT,
					  LJ_ARENA_LIFETIME_LIVE))
    return;
  life = lj_arena_lifetime_state_acq(a, cell);
  if (life == LJ_ARENA_LIFETIME_RESCUE ||
      life == LJ_ARENA_LIFETIME_LIVE ||
      (life == LJ_ARENA_LIFETIME_FREE &&
       lj_arena_sweep_state_acq(a, cell) == LJ_ARENA_SWEEP_FREEING))
    return;
  lj_assertG(0, "pre-grace destructor rollback lost exact lifetime lane");
  abort();
}

/* Claim, validate and commit one rootless typed body. No header byte is read
** until LIVE->DESTRUCT plus the paired SC admission proof has completed.
** Recovery may cancel DESTRUCT->RESCUE at any point before the FREE LP. */
static int gc2_sweep_pregrace_destruct(global_State *g, GCArena *a,
					uint32_t cell, uint32_t end,
					uint32_t kind,
					int pregrace_owned)
{
  GCSize size = gc2_sweep_pregrace_dtor_size(kind);
  GCobj *o;
  uint32_t life;
  int sweepok;
  if (!pregrace_owned || size == 0 ||
      lj_arena_ncells(size) != end - cell)
    return LJ_GC_DESTRUCT_LOST;
  if (!gc2_sweep_pregrace_obj_ready(
	a, cell, end, kind, LJ_ARENA_LIFETIME_LIVE))
    return gc2_sweep_pregrace_terminal_owned(a, cell) ?
	   LJ_GC_DESTRUCT_OWNED : LJ_GC_DESTRUCT_LOST;
  if (!lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_LIVE,
					   LJ_ARENA_LIFETIME_DESTRUCT))
    return gc2_sweep_pregrace_terminal_owned(a, cell) ?
	   LJ_GC_DESTRUCT_OWNED : LJ_GC_DESTRUCT_LOST;

  /* No-both-miss pairing with rescue admission: writer claim; SC fence; gate
  ** acquire versus reader admission RMW; SC fence; lifetime acquire. */
  la_fence_seq();
  if (!gc2_sweep_pregrace_quiet(g, a, pregrace_owned) ||
      !gc2_sweep_pregrace_obj_ready(
	a, cell, end, kind, LJ_ARENA_LIFETIME_DESTRUCT)) {
    gc2_sweep_pregrace_claim_restore(g, a, cell);
    return LJ_GC_DESTRUCT_LOST;
  }
  o = gc2_sweep_dtor_obj(g, a, cell, end, kind, 0);
  if (!o) {
    gc2_sweep_pregrace_claim_restore(g, a, cell);
    return LJ_GC_DESTRUCT_LOST;
  }

  /* Body agreement is only advisory until this final full predicate. A late
  ** publisher dirties the gate; semantic recovery steals DESTRUCT->RESCUE. */
  la_fence_seq();
  if (!gc2_sweep_pregrace_quiet(g, a, pregrace_owned) ||
      !gc2_sweep_pregrace_obj_ready(
	a, cell, end, kind, LJ_ARENA_LIFETIME_DESTRUCT)) {
    gc2_sweep_pregrace_claim_restore(g, a, cell);
    return LJ_GC_DESTRUCT_LOST;
  }
  if (!lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_DESTRUCT,
					   LJ_ARENA_LIFETIME_FREE)) {
    life = lj_arena_lifetime_state_acq(a, cell);
    if (life == LJ_ARENA_LIFETIME_RESCUE ||
	life == LJ_ARENA_LIFETIME_LIVE)
      return LJ_GC_DESTRUCT_LOST;
    if (life == LJ_ARENA_LIFETIME_FREE &&
	lj_arena_sweep_state_acq(a, cell) == LJ_ARENA_SWEEP_FREEING)
      return LJ_GC_DESTRUCT_OWNED;
    lj_assertG(0, "pre-grace destructor commit lost exact lifetime lane");
    abort();
  }
  sweepok = lj_arena_sweep_state_cas(a, cell, LJ_ARENA_SWEEP_WHITE,
					     LJ_ARENA_SWEEP_FREEING);
  lj_assertG(sweepok, "pre-grace destructor lost WHITE commit");
  if (LJ_UNLIKELY(!sweepok))
    abort();
  if (kind == LJ_ARENA_DTOR_CLOSED_UV)
    lj_func_freeuv(g, gco2uv(o));
  else
    lj_func_free(g, gco2func(o));
  return LJ_GC_DESTRUCT_ACQUIRED;
}

#define GC2_SWEEP_PREGRACE_BATCH_MAX 16u

typedef struct GC2SweepPregraceEntry {
  GCobj *o;
  uint32_t cell;
  uint32_t end;
  uint32_t kind;
} GC2SweepPregraceEntry;

typedef struct GC2SweepPregraceBatch {
  GC2SweepPregraceEntry entry[GC2_SWEEP_PREGRACE_BATCH_MAX];
  uint64_t cells;
  uint64_t state_lsb;
  uint64_t lifetime_lsb;
  uint64_t dtor[LJ_ARENA_DTOR_PLANES];
  uint64_t selected;
  uint32_t bitmap_word;
  uint32_t state_word;
  uint32_t lifetime_word;
  uint32_t n;
} GC2SweepPregraceBatch;

static LJ_AINLINE void gc2_sweep_pregrace_batch_test(global_State *g,
	GCArena *a, uint32_t path)
{
#ifdef LJ_GC2_TEST_HELPERS
  if (gc2_test_sweep_batch_hook)
    gc2_test_sweep_batch_hook(g, a, path);
#else
  UNUSED(g); UNUSED(a); UNUSED(path);
#endif
}

/* Reload and admit only exact fixed-layout starts. This does not read a body;
** its extent snapshot remains protected by the already-held arena seal. */
static void gc2_sweep_pregrace_batch_preflight(GCArena *a,
	uint32_t bitmap_word, uint64_t candidates, uint64_t p0, uint64_t p1,
	uint64_t p2, GC2SweepPregraceBatch *batch)
{
  uint32_t first_lane = lj_ffs64(candidates);
  uint32_t first_cell = (bitmap_word << 6) + first_lane;
  memset(batch, 0, sizeof(*batch));
  batch->bitmap_word = bitmap_word;
  batch->state_word = first_cell >> 5;
  batch->lifetime_word = first_cell >> 4;
  while (candidates) {
    uint32_t lane = lj_ffs64(candidates);
    uint64_t bit = (uint64_t)1 << lane;
    uint32_t cell = (bitmap_word << 6) + lane;
    uint32_t tentative_kind = (p0 & bit ? 1u : 0u) |
	(p1 & bit ? 2u : 0u) | (p2 & bit ? 4u : 0u);
    uint32_t kind = lj_arena_dtor_kind_acq(a, cell);
    GCSize size = gc2_sweep_pregrace_dtor_size(kind);
    uint32_t end;
    uint32_t i;
    int overlap = 0;
    candidates &= candidates - 1u;
    if ((cell >> 4) != batch->lifetime_word ||
	kind != tentative_kind || !gc2_sweep_dtor_kind_supported(kind) ||
	size == 0)
      continue;
    end = gc2_sweep_alloc_end(a, cell);
    if (lj_arena_ncells(size) != end - cell ||
	!gc2_sweep_pregrace_obj_ready(a, cell, end, kind,
	  LJ_ARENA_LIFETIME_LIVE))
      continue;
    for (i = 0; i < batch->n; i++) {
      GC2SweepPregraceEntry *entry = &batch->entry[i];
      if (cell < entry->end && end > entry->cell) {
	overlap = 1;
	break;
      }
    }
    if (overlap || batch->n == GC2_SWEEP_PREGRACE_BATCH_MAX)
      continue;
    {
      GC2SweepPregraceEntry *entry = &batch->entry[batch->n++];
      uint64_t state_lsb = (uint64_t)1 << ((cell & 31u) << 1);
      uint64_t lifetime_lsb = (uint64_t)1 << ((cell & 15u) << 2);
      entry->o = NULL;
      entry->cell = cell;
      entry->end = end;
      entry->kind = kind;
      batch->cells |= (uint64_t)1 << (cell & 63u);
      batch->state_lsb |= state_lsb;
      batch->lifetime_lsb |= lifetime_lsb;
      batch->selected |= bit;
      for (i = 0; i < LJ_ARENA_DTOR_PLANES; i++)
	if (kind & ((uint32_t)1u << i))
	  batch->dtor[i] |= (uint64_t)1 << (cell & 63u);
    }
  }
}

/* One exact packed predicate for every selected start. Extra unrelated lanes
** are ignored, while any selected disagreement rejects the whole batch. */
static LJ_AINLINE int gc2_sweep_pregrace_batch_ready(
	const GCArena *a, const GC2SweepPregraceBatch *batch,
	uint32_t expected_lifetime)
{
  uint64_t pair_mask = batch->state_lsb * 3u;
  uint64_t lifetime_mask = batch->lifetime_lsb * 15u;
  uint32_t i;
  if (!a || batch->n == 0 ||
      (la_load64_acq(&a->block[batch->bitmap_word]) & batch->cells) !=
	batch->cells ||
      (la_load64_acq(&a->mark[batch->bitmap_word]) & batch->cells) != 0 ||
      (la_load64_acq(&a->sweep[batch->state_word]) & pair_mask) != 0 ||
      (la_load64_acq(&a->ready[batch->bitmap_word]) & batch->cells) !=
	batch->cells ||
      (la_load64_acq(&a->root[batch->state_word]) & pair_mask) != 0 ||
      (la_load64_acq(&a->recovery[batch->state_word]) & pair_mask) != 0 ||
      (la_load64_acq(&a->lifetime[batch->lifetime_word]) &
	lifetime_mask) != batch->lifetime_lsb * expected_lifetime ||
      (la_load64_acq(&a->late[batch->bitmap_word]) & batch->cells) != 0)
    return 0;
  for (i = 0; i < LJ_ARENA_DTOR_PLANES; i++)
    if ((la_load64_acq(&a->dtor[i][batch->bitmap_word]) & batch->cells) !=
	batch->dtor[i])
      return 0;
  for (i = 0; i < batch->n; i++)
    if (!lj_arena_gc2_reclaim_clear_acq(a, batch->entry[i].cell))
      return 0;
  return 1;
}

static void gc2_sweep_pregrace_batch_restore(global_State *g, GCArena *a,
	const GC2SweepPregraceBatch *batch)
{
  uint32_t i;
  for (i = 0; i < batch->n; i++)
    gc2_sweep_pregrace_claim_restore(g, a, batch->entry[i].cell);
}

/* Claim and commit one bounded lifetime-word transaction. A zero return means
** no selected object was dispatched and every candidate remains eligible for
** the unchanged scalar retirement/pin fallback. */
static uint64_t gc2_sweep_pregrace_batch(global_State *g, GCArena *a,
	uint32_t bitmap_word, uint64_t candidates, uint64_t p0, uint64_t p1,
	uint64_t p2, int pregrace_owned)
{
  GC2SweepPregraceBatch batch;
  uint32_t i;
  if (!pregrace_owned || !g || !a || candidates == 0)
    return 0;
  gc2_sweep_pregrace_batch_preflight(
    a, bitmap_word, candidates, p0, p1, p2, &batch);
  if (batch.n == 0 ||
      !gc2_sweep_pregrace_batch_ready(
	a, &batch, LJ_ARENA_LIFETIME_LIVE) ||
      !gc2_sweep_lifetime_selected_cas(
	&a->lifetime[batch.lifetime_word], batch.lifetime_lsb,
	LJ_ARENA_LIFETIME_LIVE, LJ_ARENA_LIFETIME_DESTRUCT))
    return 0;

  gc2_sweep_pregrace_batch_test(
    g, a, LJ_GC_SWEEP_BATCH_TEST_AFTER_CLAIM);
  la_fence_seq();
  if (!gc2_sweep_pregrace_quiet(g, a, pregrace_owned) ||
      !gc2_sweep_pregrace_batch_ready(
	a, &batch, LJ_ARENA_LIFETIME_DESTRUCT))
    goto rollback;
  gc2_sweep_pregrace_batch_test(
    g, a, LJ_GC_SWEEP_BATCH_TEST_AFTER_FIRST_VALIDATE);

  for (i = 0; i < batch.n; i++) {
    GC2SweepPregraceEntry *entry = &batch.entry[i];
    entry->o = gc2_sweep_dtor_obj(
      g, a, entry->cell, entry->end, entry->kind, 0);
    if (!entry->o)
      goto rollback;
  }
  gc2_sweep_pregrace_batch_test(g, a, LJ_GC_SWEEP_BATCH_TEST_AFTER_BODY);

  la_fence_seq();
  gc2_sweep_pregrace_batch_test(
    g, a, LJ_GC_SWEEP_BATCH_TEST_BEFORE_FINAL_VALIDATE);
  if (!gc2_sweep_pregrace_quiet(g, a, pregrace_owned) ||
      !gc2_sweep_pregrace_batch_ready(
	a, &batch, LJ_ARENA_LIFETIME_DESTRUCT))
    goto rollback;
  if (!gc2_sweep_lifetime_selected_cas(
	&a->lifetime[batch.lifetime_word], batch.lifetime_lsb,
	LJ_ARENA_LIFETIME_DESTRUCT, LJ_ARENA_LIFETIME_FREE)) {
    gc2_sweep_pregrace_batch_restore(g, a, &batch);
    return 0;
  }
  if (LJ_UNLIKELY(!gc2_sweep_sweep_selected_cas(
	&a->sweep[batch.state_word], batch.state_lsb,
	LJ_ARENA_SWEEP_WHITE, LJ_ARENA_SWEEP_FREEING))) {
    lj_assertG(0, "pre-grace batch lost selected WHITE commit");
    abort();
  }
  for (i = 0; i < batch.n; i++) {
    GC2SweepPregraceEntry *entry = &batch.entry[i];
    if (entry->kind == LJ_ARENA_DTOR_CLOSED_UV)
      lj_func_freeuv(g, gco2uv(entry->o));
    else
      lj_func_free(g, gco2func(entry->o));
  }
  gc2_sweep_grace_needed_rel(g, 1);
#ifdef LJ_GC2_TEST_HELPERS
  la_add32_rlx(&gc2_test_sweep_batch_commit_count, 1);
  la_add32_rlx(&gc2_test_sweep_batch_object_count, batch.n);
#endif
  return batch.selected;

rollback:
  gc2_sweep_pregrace_batch_restore(g, a, &batch);
  return 0;
}

/* Recheck one packed candidate with the original scalar authority. A stale
** word snapshot may skip or retain work, but it never authorizes a body read,
** retirement or terminal transition. */
static void gc2_sweep_arena_unmarked_candidate(global_State *g, GCArena *a,
	uint32_t i, uint32_t tentative_kind, int pregrace_owned)
{
  uint32_t dtor_kind;
  uint32_t end = 0;
  int freed = LJ_GC_DESTRUCT_LOST;
  if (!lj_arena_bm_get(a->block, i) ||
      lj_arena_sweep_state_acq(a, i) != LJ_ARENA_SWEEP_WHITE ||
      lj_arena_bm_get(a->mark, i))
    return;
  dtor_kind = lj_arena_dtor_kind_acq(a, i);
  if (dtor_kind != tentative_kind ||
      !gc2_sweep_dtor_kind_supported(dtor_kind)) {
    /* Mixed snapshots or malformed identity retain the exact allocation. */
    (void)la_bit_test_and_set64(&a->mark[i >> 6], i & 63);
    return;
  }
  if (pregrace_owned) {
    end = gc2_sweep_alloc_end(a, i);
    freed = gc2_sweep_pregrace_destruct(
	    g, a, i, end, dtor_kind, pregrace_owned);
  }
  if (freed != LJ_GC_DESTRUCT_LOST) {
    /* ACQUIRED ran the semantic destructor; OWNED proves another terminal
    ** owner already did. Neither path creates a RETIRED counter ticket.
    ** Physical discovery, kind, READY and bytes remain for quarantine. */
    gc2_sweep_grace_needed_rel(g, 1);
    return;
  }
  /* A kind-bearing start is arena-owned exact destructor identity, not opaque
  ** raw storage. Retire it before the arena grace without reading the body. A
  ** mark overlapping classification converts RETIRED back to LIVE. */
  if (lj_arena_ready_get(a, i) &&
      lj_arena_root_state_acq(a, i) == LJ_ARENA_ROOT_NONE &&
      lj_arena_lifetime_state_acq(a, i) == LJ_ARENA_LIFETIME_LIVE) {
    (void)gc2_sweep_detached_small(g, a, i, 0);
    return;
  }
  /* A transient exact descriptor is never permission to inspect its body. */
  (void)la_bit_test_and_set64(&a->mark[i >> 6], i & 63);
}

static uint32_t gc2_sweep_arena_unmarked_bodies(global_State *g, GCArena *a,
						 int pregrace_owned)
{
  uint32_t w;
  if (!g || !a)
    return 0;
  for (w = LJ_AFIRST_CELL >> 6; w < LJ_ARENA_WORDS; w++) {
    uint64_t valid = w == (LJ_AFIRST_CELL >> 6) ?
	~(uint64_t)0 << (LJ_AFIRST_CELL & 63u) : ~(uint64_t)0;
    uint64_t block = la_load64_acq(&a->block[w]);
    uint64_t mark = la_load64_acq(&a->mark[w]);
    uint64_t sweep0 = la_load64_acq(&a->sweep[w << 1]);
    uint64_t sweep1 = la_load64_acq(&a->sweep[(w << 1) + 1u]);
    uint64_t p0 = la_load64_acq(&a->dtor[0][w]);
    uint64_t p1 = la_load64_acq(&a->dtor[1][w]);
    uint64_t p2 = la_load64_acq(&a->dtor[2][w]);
    uint64_t p3 = la_load64_acq(&a->dtor[3][w]);
    uint64_t pin, candidates;
    gc2_sweep_partition64(block, mark, sweep0, sweep1, p0, p1, p2, p3,
	valid, &pin, &candidates);
    /* Every descriptor-free or malformed WHITE start is retained. The old
    ** per-cell pin ignored its previous-bit result, so one relaxed OR preserves
    ** both semantics and marks_this_round accounting. */
    if (pin)
      (void)gc2_sweep_bulk_pin64(&a->mark[w], pin);
    {
      uint32_t group;
      for (group = 0; group < 4u; group++) {
	uint64_t group_candidates = candidates &
	  (UINT64_C(0xffff) << (group << 4));
	uint64_t committed = gc2_sweep_pregrace_batch(
	  g, a, w, group_candidates, p0, p1, p2, pregrace_owned);
	group_candidates &= ~committed;
	while (group_candidates) {
	  uint32_t lane = lj_ffs64(group_candidates);
	  uint64_t bit = (uint64_t)1 << lane;
	  uint32_t tentative_kind = (p0 & bit ? 1u : 0u) |
	    (p1 & bit ? 2u : 0u) | (p2 & bit ? 4u : 0u);
	  group_candidates &= group_candidates - 1u;
	  gc2_sweep_arena_unmarked_candidate(
	    g, a, (w << 6) + lane, tentative_kind, pregrace_owned);
	}
      }
    }
  }
  return 0;  /* Preserve the sidecar classifier's original return contract. */
}

static uint32_t gc2_sweep_arena_unmarked_impl(global_State *g, GCArena *a,
					       int allow_exclusive)
{
  uint32_t n;
  int mt_exclusive = 0;
  int smr_held = 0;
  int arena_sealed = 0;
  int pregrace_owned = 0;
  if (allow_exclusive) {
    mt_exclusive = gc2_sweep_mt_exclusive_try(g);
    if (mt_exclusive && lj_gc2_smr_read_try(g)) {
      smr_held = 1;
    } else if (mt_exclusive) {
      /* Losing the body lease only disables the optimization. The original
      ** sidecar-only classifier must still run before this arena moves from
      ** NEEDSWEEP to quarantine. */
      gc2_sweep_mt_exclusive_leave(g);
      mt_exclusive = 0;
    }
  }
  (void)lj_gc_flush_root_pending(g);
  (void)lj_gc_repair_root_spine(g);
  /* The completed SWEEP root snapshot can leave a conservative C|P admission
  ** generation behind. Only the thread which locally won both the MT gate and
  ** this exact arena seal may clear that completed intent. Keep C|S held over
  ** the bounded scan; a new bit-only publisher dirties PENDING/count first and
  ** makes every later exact commit check fail without waiting. */
  if (mt_exclusive && smr_held &&
      gc2_sweep_pregrace_cap_ready(g, a, mt_exclusive) &&
      lj_arena_reclaim_seal(a)) {
    arena_sealed = 1;
    if (lj_arena_reclaim_clear_pending(a) &&
        gc2_sweep_pregrace_cap_ready(g, a, mt_exclusive) &&
	gc2_sweep_pregrace_quiet(g, a, mt_exclusive)) {
      pregrace_owned = 1;
    } else {
      lj_arena_reclaim_unseal(a, 1);
      arena_sealed = 0;
    }
  }
  n = gc2_sweep_arena_unmarked_bodies(g, a, pregrace_owned);
  if (arena_sealed)
    lj_arena_reclaim_unseal(a, 1);
  if (smr_held)
    lj_gc2_smr_read_leave(g);
  if (mt_exclusive)
    gc2_sweep_mt_exclusive_leave(g);
  return n;
}

uint32_t lj_gc_sweep_gc2_arena_unmarked(global_State *g, GCArena *a)
{
  return gc2_sweep_arena_unmarked_impl(g, a, 0);
}

uint32_t lj_gc_sweep_gc2_arena_unmarked_exclusive(global_State *g,
						    GCArena *a)
{
  return gc2_sweep_arena_unmarked_impl(g, a, 1);
}

/* Return the next 64-cell boundary when the remaining cells in this bitmap
** word cannot require semantic post-grace work. sweep[] encodes WHITE/FREEING
** with equal low/high bits and LIVE/RETIRED with unequal bits, so the XOR is
** an exact action summary without expanding the packed lanes. Root/recovery
** ownership and late physical-free provenance retain the per-cell path.
**
** This snapshot is only an opportunistic cursor advance. A publisher which
** creates work behind it dirties the sealed generation, while quarantine
** finish independently finds LIVE/RETIRED/recovery backedges and lowers the
** cursor. It therefore cannot authorize bitmap commit or body reuse. */
static LJ_AINLINE uint32_t gc2_reclaim_noop_word_end(const GCArena *a,
						      uint32_t cell)
{
  const uint64_t pairlo = UINT64_C(0x5555555555555555);
  uint32_t w = cell >> 6;
  uint32_t lane = cell & 63u;
  uint32_t end = (w + 1u) << 6;
  uint64_t cellmask = ~(uint64_t)0 << lane;
  uint64_t block = la_load64_acq(&a->block[w]) & cellmask;
  uint64_t sweep, pairmask;

  if (block == 0)
    return end;
  if (la_load64_acq(&a->late[w]) & block)
    return cell;

  if (lane < 32u) {
    sweep = la_load64_acq(&a->sweep[w << 1]);
    pairmask = ~(uint64_t)0 << (lane << 1);
    if ((((sweep ^ (sweep >> 1)) & pairlo) |
	 la_load64_acq(&a->root[w << 1]) |
	 la_load64_acq(&a->recovery[w << 1])) & pairmask)
      return cell;
    pairmask = ~(uint64_t)0;
  } else {
    pairmask = ~(uint64_t)0 << ((lane - 32u) << 1);
  }
  sweep = la_load64_acq(&a->sweep[(w << 1) + 1u]);
  if ((((sweep ^ (sweep >> 1)) & pairlo) |
       la_load64_acq(&a->root[(w << 1) + 1u]) |
       la_load64_acq(&a->recovery[(w << 1) + 1u])) & pairmask)
    return cell;
  return end;
}

uint32_t lj_gc_reclaim_gc2_arena_ex(global_State *g, GCArena *a,
				     uint32_t limit, int *donep,
				     int *root_owner_blockedp)
{
  /* scanned counts either one ordinary cell or one proven no-op bitmap word. */
  uint32_t cell, scanned = 0, changed = 0;
  int pending = 0;
  if (donep)
    *donep = 0;
  if (root_owner_blockedp)
    *root_owner_blockedp = 0;
  if (!g || !a || limit == 0 ||
      !(lj_arena_flags_acq(a) & LJ_AF_QUARANTINE))
    return 0;
  /* The activation mirror can only veto this legacy-authorized reclaim. */
  if (lj_gc2_activation_reclaim_veto(g))
    return 0;
  cell = a->hdr.reclaim_cell;
  if (cell < LJ_AFIRST_CELL || cell > LJ_ARENA_CELLS)
    cell = LJ_AFIRST_CELL;
  while (cell < LJ_ARENA_CELLS && scanned < limit) {
    uint32_t word_end = cell;
    uint32_t state, rootmem, dtor_kind;
    /* Classify a word once per bounded pass. An actionable classification
    ** retains the original per-cell loop until the next word boundary. */
    if (scanned == 0 || (cell & 63u) == 0)
      word_end = gc2_reclaim_noop_word_end(a, cell);
    if (word_end != cell) {
      cell = word_end;
      scanned++;
      continue;
    }
    if (!lj_arena_bm_get(a->block, cell)) {
      cell++;
      scanned++;
      continue;
    }
    state = lj_arena_sweep_state_acq(a, cell);
    rootmem = lj_arena_root_state_acq(a, cell);
    dtor_kind = lj_arena_dtor_kind_acq(a, cell);
    if (rootmem == LJ_ARENA_ROOT_LINKING ||
	rootmem == LJ_ARENA_ROOT_UNLINKING) {
      /* A publisher/remover owns both the intrusive link and the allocation
      ** reuse veto. Reclaim never waits for it or inspects a mutable header. */
      if (root_owner_blockedp)
	*root_owner_blockedp = 1;
      pending = 1;
      cell++;
      scanned++;
      continue;
    }
    if (rootmem == LJ_ARENA_ROOT_MEMBER &&
	state == LJ_ARENA_SWEEP_RETIRED) {
      /* A finalizer/resurrection publisher can win after detachment. Convert
      ** the destructor ticket back to the ordinary LIVE reanchor completion
      ** state; MEMBER proves that reinsertion has already happened exactly
      ** once. */
      if (lj_arena_sweep_state_cas(a, cell, LJ_ARENA_SWEEP_RETIRED,
					   LJ_ARENA_SWEEP_LIVE)) {
	uint32_t old = lj_arena_reclaim_deferred_sub(a, 1);
	lj_assertG(old != 0, "member rescue deferred underflow");
	UNUSED(old);
	state = LJ_ARENA_SWEEP_LIVE;
	changed++;
      } else {
	pending = 1;
	cell++;
	scanned++;
	continue;
      }
    }
    if (lj_arena_late_get(a, cell)) {
      uint32_t life;
      if (rootmem != LJ_ARENA_ROOT_NONE) {
	/* A terminal body publication conflicting with membership is corrupt but
	** not permission to free or rewrite ownership. Keep the arena quarantined. */
	pending = 1;
	cell++;
	scanned++;
	continue;
      }
      /* late[] is irrevocable logical-free provenance and type-specific GC
      ** callers publish it only after their physical destructor. Once the
      ** quarantined owner observes that every side lane is idle, commit the
      ** exact terminal lifetime and classify it FREEING. Keeping it WHITE
      ** would make bitmap finalization retain a dead body for another whole
      ** generation and contradict FREEING=>FREE reuse admission. */
      if (lj_arena_recovery_state_acq(a, cell) !=
	  LJ_ARENA_RECOVERY_IDLE) {
	pending = 1;
	cell++;
	scanned++;
	continue;
      }
      if (!lj_arena_gc2_reclaim_clear_acq(a, cell)) {
	pending = 1;
	cell++;
	scanned++;
	continue;
      }
      life = lj_arena_lifetime_state_acq(a, cell);
      if (life == LJ_ARENA_LIFETIME_LIVE) {
	if (!lj_arena_lifetime_state_cas(a, cell,
	      LJ_ARENA_LIFETIME_LIVE, LJ_ARENA_LIFETIME_DESTRUCT)) {
	  pending = 1;
	  cell++;
	  scanned++;
	  continue;
	}
	if (!(lj_arena_flags_acq(a) & LJ_AF_QUARANTINE) ||
	    !lj_arena_bm_get(a->block, cell) ||
	    !lj_arena_late_get(a, cell) ||
	    lj_arena_root_state_acq(a, cell) != LJ_ARENA_ROOT_NONE ||
	    lj_arena_recovery_state_acq(a, cell) !=
	      LJ_ARENA_RECOVERY_IDLE ||
	    !lj_arena_gc2_reclaim_clear_acq(a, cell)) {
	  (void)lj_arena_lifetime_state_cas(a, cell,
	    LJ_ARENA_LIFETIME_DESTRUCT, LJ_ARENA_LIFETIME_LIVE);
	  pending = 1;
	  cell++;
	  scanned++;
	  continue;
	}
	if (!lj_arena_lifetime_state_cas(a, cell,
	      LJ_ARENA_LIFETIME_DESTRUCT, LJ_ARENA_LIFETIME_FREE)) {
	  pending = 1;
	  cell++;
	  scanned++;
	  continue;
	}
	life = LJ_ARENA_LIFETIME_FREE;
      }
      if (life != LJ_ARENA_LIFETIME_FREE) {
	pending = 1;
	cell++;
	scanned++;
	continue;
      }
      while (state != LJ_ARENA_SWEEP_FREEING) {
	uint32_t oldstate = state;
	if (lj_arena_sweep_state_cas(a, cell, oldstate,
					 LJ_ARENA_SWEEP_FREEING)) {
	  if (oldstate == LJ_ARENA_SWEEP_RETIRED) {
	    uint32_t old = lj_arena_reclaim_deferred_sub(a, 1);
	    lj_assertG(old != 0, "late-terminal deferred underflow");
	    UNUSED(old);
	  }
	  changed++;
	  state = LJ_ARENA_SWEEP_FREEING;
	  break;
	}
	state = lj_arena_sweep_state_acq(a, cell);
      }
      cell++;
      scanned++;
      continue;
    }
    if (state == LJ_ARENA_SWEEP_RETIRED &&
	((la_load64_acq(&a->mark[cell >> 6]) >> (cell & 63)) & 1u) &&
	lj_arena_sweep_state_cas(a, cell, LJ_ARENA_SWEEP_RETIRED,
					 LJ_ARENA_SWEEP_LIVE)) {
      uint32_t old = lj_arena_reclaim_deferred_sub(a, 1);
      lj_assertG(old != 0, "sealed marked rescue underflow");
      UNUSED(old);
      state = LJ_ARENA_SWEEP_LIVE;
      changed++;
    }
    if (state == LJ_ARENA_SWEEP_LIVE &&
	rootmem == LJ_ARENA_ROOT_NONE &&
	dtor_kind != LJ_ARENA_DTOR_NONE) {
      /* A late semantic mark rescued this arena-owned body after retirement.
      ** Its immutable sidecar remains the next-cycle discovery identity, so it
      ** must return directly to WHITE instead of being inserted into the
      ** ownership spine. The rescue path sets mark before publishing LIVE.
      ** Any inconsistent descriptor is retained fail-closed in the same way. */
      if (!gc2_sweep_dtor_kind_supported(dtor_kind) ||
	  !lj_arena_ready_get(a, cell) ||
	  lj_arena_lifetime_state_acq(a, cell) !=
	    LJ_ARENA_LIFETIME_LIVE)
	(void)la_bit_test_and_set64(&a->mark[cell >> 6], cell & 63);
      if (lj_arena_sweep_state_cas(a, cell, LJ_ARENA_SWEEP_LIVE,
					  LJ_ARENA_SWEEP_WHITE))
	changed++;
      else
	pending = 1;
      cell++;
      scanned++;
      continue;
    }
    if (state == LJ_ARENA_SWEEP_LIVE ||
	state == LJ_ARENA_SWEEP_RETIRED) {
      uint32_t end = gc2_sweep_alloc_end(a, cell);
      GCobj *o = gc2_sweep_cell_obj(g, a, cell, end);
      if (!o) {
	if (rootmem != LJ_ARENA_ROOT_NONE) {
	  (void)la_bit_test_and_set64(&a->mark[cell >> 6], cell & 63);
	  pending = 1;
	  cell++;
	  scanned++;
	  continue;
	}
	/* A valid detach publishes only an exact ownership-spine header, but a
	** hostile/racy producer may leave a header snapshot temporarily or
	** permanently undecodable. Fail closed without parking the whole GC: pin
	** the allocation as graphless raw storage and settle detached accounting.
	** A later semantic edge can validate/mark the same block again; reclamation
	** never reuses it based on attacker-controlled header bytes. */
	(void)la_bit_test_and_set64(&a->mark[cell >> 6], cell & 63);
	if (lj_arena_sweep_state_cas(a, cell, state,
					 LJ_ARENA_SWEEP_WHITE)) {
	  if (state == LJ_ARENA_SWEEP_RETIRED) {
	    uint32_t old = lj_arena_reclaim_deferred_sub(a, 1);
	    lj_assertG(old != 0, "invalid-header deferred underflow");
	    UNUSED(old);
	  }
	  changed++;
	} else {
	  pending = 1;
	}
	cell++;
	scanned++;
	continue;
      }
      if (state == LJ_ARENA_SWEEP_LIVE) {
	if (rootmem == LJ_ARENA_ROOT_MEMBER) {
	  /* A previous pass committed the root edge before losing the LIVE->WHITE
	  ** race. Finish only the accounting state; touching gcw would duplicate
	  ** the intrusive membership. */
	  if (lj_arena_sweep_state_cas(a, cell, LJ_ARENA_SWEEP_LIVE,
					      LJ_ARENA_SWEEP_WHITE))
	    changed++;
	  else
	    pending = 1;
	  cell++;
	  scanned++;
	  continue;
	}
	if (o->gch.gct == 0) {
	  int owned = lj_arena_destruct_acquire(
	    lj_arena_cellptr(a, cell),
	    (size_t)(end - cell) << LJ_CELL_SHIFT);
	  if (owned != LJ_ARENA_DESTRUCT_LOST)
	    changed++;
	  else
	    pending = 1;
	  cell++;
	  scanned++;
	  continue;
	}
#if LJ_HASJIT
	if (o->gch.gct == (uint32_t)~LJ_TTRACE &&
	    la_load64_acq(&gco2trace(o)->retire_epoch) != 0) {
	  /* A retire-list root deliberately marked this trace during the grace.
	  ** It is not a rescued Lua root: leave it detached until the token-owned
	  ** physical destructor publishes gct=0/FREEING. */
	  (void)lj_trace_free_gc(g, gco2trace(o));
	  gc2_sweep_grace_needed_rel(g, 1);
	  pending = 1;
	  cell++;
	  scanned++;
	  continue;
	}
#endif
	/* The detach left gcw untouched for pre-grace readers. Claim persistent
	** membership before rewriting it, publish once, then retire LIVE. */
	{
	  GCRootStateRef rootstate;
	  int link = gc_root_link_claim_at(
	    g, o, lj_arena_cellptr(a, cell), &rootstate);
	  if (link == LJ_GC_ROOT_LINK_ALREADY) {
	    if (lj_arena_sweep_state_cas(a, cell, LJ_ARENA_SWEEP_LIVE,
						LJ_ARENA_SWEEP_WHITE))
	      changed++;
	    else
	      pending = 1;
	  } else if (link != LJ_GC_ROOT_LINKED) {
	    pending = 1;
	  } else if (gc_root_publish_claimed(g, o, &rootstate) !=
		     LJ_GC_ROOT_LINKED) {
	    pending = 1;
	  } else {
	    gc_test_root_state(g, o,
			       LJ_GC_ROOT_STATE_TEST_SMALL_REANCHOR_LINKED);
	    if (lj_arena_sweep_state_cas(a, cell, LJ_ARENA_SWEEP_LIVE,
						LJ_ARENA_SWEEP_WHITE))
	      changed++;
	    else
	      pending = 1;
	  }
	}
      } else {
        if (o->gch.gct == 0) {
	  int owned = lj_arena_destruct_acquire(
	    lj_arena_cellptr(a, cell),
	    (size_t)(end - cell) << LJ_CELL_SHIFT);
	  if (owned != LJ_ARENA_DESTRUCT_LOST) {
	    changed++;
	  } else {
	    pending = 1;
	  }
	  cell++;
	  scanned++;
	  continue;
	}
#if LJ_HASJIT
	if (o->gch.gct == (uint32_t)~LJ_TTRACE) {
	  /* Listing/slot teardown is not physical completion. Keep RETIRED until
	  ** trace_freebody tears down exittab and release-publishes gct=0. */
	  if (!lj_trace_body_destroyed_acq(gco2trace(o)))
	    (void)lj_trace_free_gc(g, gco2trace(o));
	  gc2_sweep_grace_needed_rel(g, 1);
	  pending = 1;
	} else
#endif
	{
	  int freed = gc2_free_unmarked_obj(g, o, 0, 1);
	  if (freed == LJ_GC_DESTRUCT_LOST)
	    pending = 1;  /* DESTRUCT->RESCUE or another nonterminal owner won. */
	  else
	    changed++;  /* ACQUIRED ran it; OWNED proves a prior terminal owner. */
	}
      }
    }
    cell++;
    scanned++;
  }
  a->hdr.reclaim_cell = cell;
  if (cell == LJ_ARENA_CELLS) {
    uint32_t deferred = lj_arena_reclaim_deferred_acq(a);
    if (deferred == 0 && !pending &&
	lj_arena_gcprep_pending_acq(a) == 0) {
      if (donep)
	*donep = 1;
    } else {
      a->hdr.reclaim_cell = LJ_AFIRST_CELL;
    }
  }
  /* Cursor advancement is bounded real work and must keep the owner scheduled
  ** until it reaches EOF. At EOF, a token-busy trace returns zero so the worker
  ** can park rather than spin; the active-phase timeout retries the pass after
  ** token release. */
  if (changed)
    return changed;
  return cell < LJ_ARENA_CELLS && scanned != 0 ? 1u : 0u;
}

uint32_t lj_gc_reclaim_gc2_arena(global_State *g, GCArena *a,
				  uint32_t limit, int *donep)
{
  return lj_gc_reclaim_gc2_arena_ex(g, a, limit, donep, NULL);
}

static void gc2_huge_sweep_reader_drop(LJHugeReader *reader, int *pendingp)
{
  int released;
  if (!reader || !reader->h)
    return;
  released = lj_arena_hugetab_reader_release(reader, NULL);
  if (released != LJ_ARENA_HUGE_READER_RELEASED && pendingp)
    *pendingp = 1;
}

uint32_t lj_gc_reclaim_gc2_huge(global_State *g, TGState *tg, void *p,
				 const LJHugeInfo *hi, int *pendingp)
{
  GCArena *a;
  GCobj *o;
  LJHugeReader view = { NULL, NULL, 0 };
  LJHugeInfo fresh;
  uint32_t result = 0;
  uint32_t flags, rootmem;
  int rootq, admitted;
  if (pendingp)
    *pendingp = 0;
  if (!g || !tg || !p || !hi)
    return 0;
  /* Keep the mapping and its ticket intact on any typed/legacy disagreement. */
  if (lj_gc2_activation_reclaim_veto(g)) {
    if (pendingp)
      *pendingp = 1;
    return 0;
  }
  memset(&fresh, 0, sizeof(fresh));
  admitted = lj_arena_hugetab_sweep_reader_acquire(
    &tg->huge, p, &view, &fresh);
  if (admitted != LJ_ARENA_HUGE_READER_ACQUIRED) {
    if (pendingp)
      *pendingp = 1;
    return 0;
  }
#define LJ_GC2_HUGE_DONE(value) \
  do { result = (uint32_t)(value); goto huge_done; } while (0)
  a = lj_arena_of(p);
  flags = fresh.flags;  /* Iterator metadata is a hint, never body authority. */
  if (flags & LJ_HUGEF_BUSY) {
    if (pendingp) *pendingp = 1;
    LJ_GC2_HUGE_DONE(0);  /* Publisher/reanchor owns all header access. */
  }
  o = (flags & LJ_HUGEF_TICKET) ?
      (GCobj *)la_loadptr_acq((void *const *)&a->hdr.retire_obj) : NULL;
  rootq = lj_arena_hugetab_root_state_acq(&tg->huge, p, NULL);
  if (rootq < 0) {
    if (pendingp) *pendingp = 1;
    LJ_GC2_HUGE_DONE(0);
  }
  rootmem = (uint32_t)rootq;
  if (rootmem == LJ_ARENA_ROOT_LINKING ||
      rootmem == LJ_ARENA_ROOT_UNLINKING) {
    /* A preempted root owner pins reuse by itself. Preserve this mapping for
    ** the current sweep and let table progress continue; the owner will later
    ** create a valid detach ticket or leave reclamation to a future cycle. */
    if (!(flags & LJ_HUGEF_MARK)) {
      (void)lj_arena_hugetab_mark(&tg->huge, p, NULL);
      LJ_GC2_HUGE_DONE(1);
    }
    LJ_GC2_HUGE_DONE(0);
  }
  if (rootmem == LJ_ARENA_ROOT_MEMBER &&
      (flags & (LJ_HUGEF_MARK|LJ_HUGEF_TICKET)) !=
	(LJ_HUGEF_MARK|LJ_HUGEF_TICKET)) {
    /* A stable retained fixed/FINREG/direct-publisher mapping legitimately has
    ** no detach ticket. Preserve it for this sweep without manufacturing LIVE
    ** reanchor work or keeping the huge sweep pending forever. */
    if (!(flags & LJ_HUGEF_MARK)) {
      (void)lj_arena_hugetab_mark(&tg->huge, p, NULL);
      LJ_GC2_HUGE_DONE(1);
    }
    LJ_GC2_HUGE_DONE(0);
  }

  if (flags & LJ_HUGEF_FREEING) {
    uint64_t retire_epoch = la_load64_acq(&a->hdr.retire_epoch);
    uint64_t now = lj_gc2_retire_epoch(g);
    if (retire_epoch == ~(uint64_t)0) {
      la_store64_rel(&a->hdr.retire_epoch, now);
      gc2_sweep_grace_needed_rel(g, 1);
      if (pendingp) *pendingp = 1;
      LJ_GC2_HUGE_DONE(1);
    }
    if (retire_epoch >= now) {
      if (pendingp) *pendingp = 1;
      LJ_GC2_HUGE_DONE(0);
    }
  }

  if (flags & LJ_HUGEF_MARK) {
    if (o && (flags & LJ_HUGEF_TICKET)) {
      GCRootStateRef rootstate;
      int link;
#if LJ_HASJIT
      if (o->gch.gct == (uint32_t)~LJ_TTRACE &&
	  la_load64_acq(&gco2trace(o)->retire_epoch) != 0) {
	gc2_huge_sweep_reader_drop(&view, pendingp);
	(void)lj_trace_free_gc(g, gco2trace(o));
	gc2_sweep_grace_needed_rel(g, 1);
	if (pendingp) *pendingp = 1;
	LJ_GC2_HUGE_DONE(0);
      }
#endif
      if (!gc2_valid_freeable_obj(g, o, 1)) {
	if (pendingp) *pendingp = 1;
	LJ_GC2_HUGE_DONE(0);
      }
      if (rootmem == LJ_ARENA_ROOT_MEMBER) {
	/* A prior pass committed the exact intrusive edge and only lost the
	** ticket-finishing tail. Never insert it again. */
	gc2_huge_sweep_reader_drop(&view, pendingp);
	if (!lj_arena_hugetab_claim_live_ticket(&tg->huge, p, NULL)) {
	  if (pendingp) *pendingp = 1;
	  LJ_GC2_HUGE_DONE(0);
	}
	la_storeptr_rel(&a->hdr.retire_obj, NULL);
	if (!lj_arena_hugetab_finish_live_ticket(&tg->huge, p, NULL)) {
	  lj_assertG(0, "huge live ticket lost after member reconciliation");
	  if (pendingp) *pendingp = 1;
	  LJ_GC2_HUGE_DONE(0);
	}
	LJ_GC2_HUGE_DONE(1);
      }
      link = gc_root_link_claim_at(g, o, p, &rootstate);
      if (link == LJ_GC_ROOT_LINK_ALREADY) {
	/* Another publisher completed between the fresh state sample and claim. */
	gc2_huge_sweep_reader_drop(&view, pendingp);
	if (!lj_arena_hugetab_claim_live_ticket(&tg->huge, p, NULL)) {
	  if (pendingp) *pendingp = 1;
	  LJ_GC2_HUGE_DONE(0);
	}
	la_storeptr_rel(&a->hdr.retire_obj, NULL);
	if (!lj_arena_hugetab_finish_live_ticket(&tg->huge, p, NULL)) {
	  lj_assertG(0, "huge live ticket lost after concurrent member");
	  if (pendingp) *pendingp = 1;
	  LJ_GC2_HUGE_DONE(0);
	}
	LJ_GC2_HUGE_DONE(1);
      }
      if (link != LJ_GC_ROOT_LINKED) {
	if (pendingp) *pendingp = 1;
	LJ_GC2_HUGE_DONE(0);
      }
      /* Membership must precede BUSY: after BUSY, a losing claim cannot safely
      ** roll ownership back through a mapping operation it does not own. */
      gc2_huge_sweep_reader_drop(&view, pendingp);
      if (!lj_arena_hugetab_claim_live_ticket(&tg->huge, p, NULL)) {
	gc_root_link_rollback(g, &rootstate);
	if (pendingp) *pendingp = 1;
	LJ_GC2_HUGE_DONE(0);
      }
      /* Every old nonfixed root was detached before the grace. One mapping has
      ** one allocation, so retire_obj is an exact single reanchor ticket. */
      if (gc_root_publish_claimed(g, o, &rootstate) !=
	  LJ_GC_ROOT_LINKED) {
	if (pendingp) *pendingp = 1;
	LJ_GC2_HUGE_DONE(0);
      }
      la_storeptr_rel(&a->hdr.retire_obj, NULL);
      {
	int finished = lj_arena_hugetab_finish_live_ticket(&tg->huge, p, NULL);
	lj_assertG(finished, "huge live ticket lost after root reanchor");
	if (!finished) {
	  if (pendingp) *pendingp = 1;
	  LJ_GC2_HUGE_DONE(0);
	}
      }
      LJ_GC2_HUGE_DONE(1);
    }
    LJ_GC2_HUGE_DONE(0);  /* Marked raw/fixed storage was never detached. */
  }

  if (!(flags & (LJ_HUGEF_RETIRED|LJ_HUGEF_FREEING))) {
    int ticketed;
    gc2_huge_sweep_reader_drop(&view, pendingp);
    ticketed = o ? lj_arena_hugetab_retire(
	&tg->huge, p, o, lj_gc2_retire_epoch(g), NULL) : 0;
    if (ticketed) {
      /* Return 2 means the final TICKET contains MARK. HugeTab deliberately
      ** does not encode whether that mark preceded BUSY or arrived as an
      ** opaque intent, so discharge even if this duplicates an earlier walk. */
      if (ticketed == 2)
	(void)lj_gc2_trace_sweep_root(g, o);
      gc2_sweep_grace_needed_rel(g, 1);
      if (pendingp) *pendingp = 1;
      LJ_GC2_HUGE_DONE(1);
    }
    /* Opaque huge storage has no GC destructor proof. Preserve it rather than
    ** interpreting payload bytes as a header. */
    (void)lj_arena_hugetab_mark(&tg->huge, p, NULL);
    LJ_GC2_HUGE_DONE(1);
  }

  if (!o) {
    /* An external subsystem owns the logical free. FREEING is enough to let
    ** this sole huge-table owner perform the final delete after the grace. */
    if (!(flags & LJ_HUGEF_FREEING)) {
      gc2_huge_sweep_reader_drop(&view, pendingp);
      if (
	!lj_arena_hugetab_claim_freeing(&tg->huge, p, NULL)) {
	  if (pendingp) *pendingp = 1;
	  LJ_GC2_HUGE_DONE(0);
      }
    }
  } else {
#if LJ_HASJIT
    if (o->gch.gct == (uint32_t)~LJ_TTRACE &&
	!lj_trace_body_destroyed_acq(gco2trace(o))) {
      gc2_huge_sweep_reader_drop(&view, pendingp);
      (void)lj_trace_free_gc(g, gco2trace(o));
      gc2_sweep_grace_needed_rel(g, 1);
      if (pendingp) *pendingp = 1;
      LJ_GC2_HUGE_DONE(0);
    }
#endif
    if (!(flags & LJ_HUGEF_FREEING)) {
      gc2_huge_sweep_reader_drop(&view, pendingp);
      if (!lj_arena_hugetab_claim_freeing(&tg->huge, p, NULL)) {
	if (pendingp) *pendingp = 1;
	LJ_GC2_HUGE_DONE(0);
      }
      if (o->gch.gct != 0 &&
	  gc2_free_unmarked_obj(g, o, 1, 1) == LJ_GC_DESTRUCT_LOST) {
	(void)lj_arena_hugetab_revert_retired(&tg->huge, p);
	if (pendingp) *pendingp = 1;
	LJ_GC2_HUGE_DONE(0);
      }
    }
  }
  gc2_huge_sweep_reader_drop(&view, pendingp);
  {
    LJHugeInfo snap;
    if (lj_arena_hugetab_delete(&tg->huge, p, &snap) == 1) {
      /* No access to a/a->hdr is legal after this unmap. */
      lj_arena_huge_unmap_claimed(p, snap.size);
      LJ_GC2_HUGE_DONE(1);
    }
  }
  LJ_GC2_HUGE_DONE(0);
huge_done:
  gc2_huge_sweep_reader_drop(&view, pendingp);
#undef LJ_GC2_HUGE_DONE
  return result;
}

/* Check whether we can clear a key or a value slot from a table. */
static int gc_mayclear(global_State *g, cTValue *o, int val)
{
  /*
  ** Weak clearing reads slots without owning their table generation. A stale
  ** tagged GC pointer whose header has been reused is not a live weak edge.
  ** Clear it instead of dereferencing reclaimed header state.
  */
  if (LJ_UNLIKELY(!lj_gc2_tv_gcref_valid_edge(g, o)))
    return 1;
  if (tvisgcv(o)) {  /* Only collectable objects can be weak references. */
    if (tvisstr(o)) {  /* But strings cannot be used as weak references. */
      (void)lj_gc2_markobj(g, gcV(o));  /* Interned strings remain strong. */
      return 0;
    }
    if (lj_gc2_ismarked(g, gcV(o)) <= 0)
      return 1;  /* Object is about to be collected. */
    if (tvisudata(o) && val && isfinalized(udataV(o)))
      return 1;  /* Finalized userdata is dropped only from values. */
  }
  return 0;  /* Cannot clear. */
}

/* Clear collected entries from weak tables. */
void lj_gc_clearweak_bridge(global_State *g, GCobj *o)
{
  LJGC2Lease leases[2];
  uint32_t leaseidx = 0;
  memset(leases, 0, sizeof(leases));
  if (!o)
    return;
  lj_gc2_smr_read_enter(g);
  if (lj_gc2_obj_lease_acquire(g, o, (uint32_t)~LJ_TTAB,
			       NULL, &leases[leaseidx]) < 0) {
    lj_gc2_smr_read_leave(g);
    return;
  }
  while (o) {
    GCtab *t = gco2tab(o);
    GCobj *next;
    /* Weak-list membership is semantic ownership, but it is not a counted
    ** body admission and does not pin a concurrently retired vector. Current
    ** TAB admission plus SMR cover every slot and gclist dereference. */
    lj_assertG((lj_obj_gcflags(obj2gco(t)) & LJ_GC_WEAK),
	       "clear of non-weak table");
    if ((lj_obj_gcflags(obj2gco(t)) & LJ_GC_WEAKVAL)) {
      TValue *array;
      MSize i, asize, acap;
      if (lj_tab_array_snapshot_gc_held(g, t, &array, &asize, &acap) ==
	  LJ_TAB_GC_SNAPSHOT_OK) {
	UNUSED(acap);
	for (i = 0; i < asize; i++) {
	  /* Clear array slot when value is about to be collected. */
	  TValue val;
	  TValue *slot = &array[i];
	  lj_tv_load_acq(&val, &array[i]);
	  if (tvisforward(&val)) {
	    slot = lj_tab_forwarded_array_slot(t, array, asize, i, &val);
	    if (!slot)
	      continue;
	  }
	  if (gc_mayclear(g, &val, 1)) {
	    TValue key;
	    setintV(&key, (int32_t)i);
	    (void)lj_tab_clear_weak_slot_keyed(g, t, slot, &key, &val);
	  }
	}
      }
    }
    {
      MSize i, hmask;
      Node *node;
      if (lj_tab_node_snapshot_gc_held(g, t, &node, &hmask) ==
	  LJ_TAB_GC_SNAPSHOT_OK) {
	for (i = 0; i <= hmask; i++) {
	  Node *n = &node[i];
	  TValue key, val;
	  TValue *slot = &n->val;
	  int key_loaded = 0;
	  /* Clear hash slot when key or value is about to be collected. */
	  lj_tv_load_acq(&val, &n->val);
	  if (tvisforward(&val)) {
	    lj_tv_load_acq(&key, &n->key);
	    key_loaded = 1;
	    while (tviskeylock(&key)) {
	      gc_root_wait_no_l();
	      lj_tv_load_acq(&key, &n->key);
	    }
	    if (tvisnil(&key))
	      continue;
	    slot = lj_tab_forwarded_hash_slot(t, node, hmask, &key, &val);
	    if (!slot)
	      continue;
	  }
	  if (!tvisnil(&val)) {
	    if (!key_loaded)
	      lj_tv_load_acq(&key, &n->key);
	    while (tviskeylock(&key)) {
	      gc_root_wait_no_l();
	      lj_tv_load_acq(&key, &n->key);
	    }
	    if (gc_mayclear(g, &key, 0) || gc_mayclear(g, &val, 1))
	      (void)lj_tab_clear_weak_slot_keyed(g, t, slot, &key, &val);
	  }
	}
      }
    }
    next = lj_tab_gclist_acq(t);
    /* Acquire the successor before dropping either certificate for current.
    ** A load/release/next-iteration acquire gap permits a completed grace to
    ** destroy and reuse next even though its raw address remains in hand. */
    if (next && lj_gc2_obj_lease_acquire(
		  g, next, (uint32_t)~LJ_TTAB, NULL,
		  &leases[leaseidx ^ 1u]) < 0) {
      lj_gc2_lease_release(&leases[leaseidx]);
      lj_gc2_smr_read_leave(g);
      return;
    }
    lj_gc2_lease_release(&leases[leaseidx]);
    lj_gc2_smr_read_leave(g);
    o = next;
    leaseidx ^= 1u;
    if (o)
      lj_gc2_smr_read_enter(g);
  }
}

typedef struct GCShutdownSeen {
  const void **slot;
  size_t mask;
  size_t count;
} GCShutdownSeen;

static size_t gc_shutdown_ptrhash(const void *p)
{
  uintptr_t x = (uintptr_t)p >> 3;
#if LJ_64
  x ^= x >> 33;
  x *= (uintptr_t)0xff51afd7ed558ccdULL;
  x ^= x >> 33;
#else
  x ^= x >> 16;
  x *= (uintptr_t)0x7feb352dUL;
  x ^= x >> 15;
#endif
  return (size_t)x;
}

static int gc_shutdown_seen_grow(GCShutdownSeen *seen, size_t minslots)
{
  const void **old = seen->slot;
  size_t oldcap = seen->mask + 1u;
  size_t cap = old ? oldcap << 1 : 1024u;
  const void **slot;
  size_t i;
  while (cap < minslots && cap <= (SIZE_MAX >> 1))
    cap <<= 1;
  if (cap < minslots || cap > SIZE_MAX / sizeof(*slot))
    return 0;
  slot = (const void **)calloc(cap, sizeof(*slot));
  if (!slot)
    return 0;
  if (old) {
    for (i = 0; i < oldcap; i++) {
      const void *p = old[i];
      if (p) {
	size_t j = gc_shutdown_ptrhash(p) & (cap - 1u);
	while (slot[j])
	  j = (j + 1u) & (cap - 1u);
	slot[j] = p;
      }
    }
    free((void *)old);
  }
  seen->slot = slot;
  seen->mask = cap - 1u;
  return 1;
}

/* Return 1 for a first observation, 0 for a duplicate/cycle, -1 on OOM. */
static int gc_shutdown_seen_add(GCShutdownSeen *seen, const void *p)
{
  size_t i;
  if (!seen->slot && !gc_shutdown_seen_grow(seen, 1024u))
    return -1;
  if (seen->count >= (seen->mask + 1u) / 2u &&
      !gc_shutdown_seen_grow(seen, (seen->mask + 1u) << 1))
    return -1;
  i = gc_shutdown_ptrhash(p) & seen->mask;
  while (seen->slot[i]) {
    if (seen->slot[i] == p)
      return 0;
    i = (i + 1u) & seen->mask;
  }
  seen->slot[i] = p;
  seen->count++;
  return 1;
}

static void gc_shutdown_seen_clear(GCShutdownSeen *seen)
{
  if (seen->slot)
    memset((void *)seen->slot, 0, (seen->mask + 1u) * sizeof(*seen->slot));
  seen->count = 0;
}

static void gc_shutdown_seen_fini(GCShutdownSeen *seen)
{
  free((void *)seen->slot);
  seen->slot = NULL;
  seen->mask = seen->count = 0;
}

typedef struct GC2ShutdownDestroy {
  LJGCDestructCtx dctx;
  GCFreeFunc fn;
  void *base;
  GCSize size;
  uint32_t gct;
  uint8_t deferred;
  uint8_t acquired;
} GC2ShutdownDestroy;

static int gc2_shutdown_reconcile_base(global_State *g, void *base)
{
  GCArena *a;
  if (g->allocf != lj_arena_allocf ||
      la_load32_acq(&g->allocf_arena) == 0)
    return 1;
  if (!base || !checkptrGC(base) ||
      !lj_gc2_mem_registered_known(g, base))
    return 0;
  a = lj_arena_of(base);
  /* Huge lifetime/destructor arbitration lives entirely in its HugeTab slot;
  ** its mapped header is not a small-arena C|P admission generation. */
  return lj_arena_ishuge(a) ? 1 : lj_arena_terminal_reconcile(a);
}

/* Validate immutable destructor geometry while the incoming root edge still
** makes the body discoverable. Lifetime acquisition is deliberately separate:
** it can run only after the terminal root side-plane has reached NONE. */
static int gc2_shutdown_destroy_init(global_State *g, GCobj *o,
				      GC2ShutdownDestroy *sd)
{
  memset(sd, 0, sizeof(*sd));
  if (!gc2_valid_freeable_obj(g, o, 1))
    return 0;
  sd->gct = o->gch.gct;
  sd->base = o;
  sd->deferred = (uint8_t)gc2_deferred_body_pending(g, o);
#if LJ_HASJIT
  if (sd->gct == (uint32_t)~LJ_TTRACE)
    return 1;  /* Retire-list publication is the trace prepare transaction. */
#endif
  if (sd->gct < (uint32_t)~LJ_TSTR ||
      sd->gct > (uint32_t)~LJ_TUDATA ||
      !(sd->fn = gc_freefunc[sd->gct - (uint32_t)~LJ_TSTR]) ||
      !gc2_free_body_info(g, o, &sd->base, &sd->size))
    return 0;
  return 1;
}

static int gc2_shutdown_destroy_acquire(global_State *g,
					 GC2ShutdownDestroy *sd)
{
  int acquired;
  /* A prior destructor can recreate count-zero C|P after the round-wide PRE.
  ** Reconcile this exact allocation's arena at the last point before its
  ** irreversible lifetime acquisition. */
  if (!gc2_shutdown_reconcile_base(g, sd->base))
    abort();
  acquired = lj_gc_destructor_enter(g, sd->base, sd->size, &sd->dctx);
  if (acquired == LJ_GC_DESTRUCT_LOST)
    return 0;
  sd->acquired = (uint8_t)acquired;
  return 1;
}

static void gc2_shutdown_destroy_commit(global_State *g, GCobj *o,
					 GC2ShutdownDestroy *sd)
{
  if (sd->acquired != LJ_GC_DESTRUCT_ACQUIRED)
    return;  /* A previous terminal owner already completed this body. */
  sd->fn(g, o);
  lj_gc_destructor_leave(g, &sd->dctx);
  if (sd->deferred)
    la_store8_rel(&o->gch.gct, 0);  /* Arena body awaits terminal unmap. */
}

/* All runtime publishers/readers have joined before freeall. Reconcile the
** exact side-plane claim after its incoming edge is removed and before a
** destructor can reach the arena allocator. This is the only terminal path
** allowed to complete a preempted LINKING/UNLINKING state. */
static int gc2_shutdown_release_root_state(global_State *g, GCobj *o)
{
  GCRootStateRef rootstate;
  uint32_t state;
  int kind = gc_root_state_ref(g, o, &rootstate);
  if (kind == GC_ROOT_STATE_INVALID)
    return 0;
  if (kind == GC_ROOT_STATE_EXEMPT)
    return 1;
  state = gc_root_state_acq(&rootstate);
  if (state == LJ_ARENA_ROOT_NONE)
    return 1;  /* Unconverted direct-VM publication. */
  if (state == LJ_ARENA_ROOT_MEMBER) {
    if (!gc_root_state_cas(&rootstate, LJ_ARENA_ROOT_MEMBER,
				   LJ_ARENA_ROOT_UNLINKING))
      return 0;
    state = LJ_ARENA_ROOT_UNLINKING;
  }
  if (state == LJ_ARENA_ROOT_LINKING)
    return gc_root_link_terminal_rollback(g, &rootstate);
  if (state == LJ_ARENA_ROOT_UNLINKING)
    return gc_root_unlink_commit(g, &rootstate);
  return 0;
}

/* Destructor acquisition is allowed to lose nonblocking ownership, but the
** incoming root edge has deliberately not been spliced yet. Recreate the exact
** MEMBER side-plane through the ordinary small/huge lifetime claim so the next
** terminal round can safely validate and retry this same body. */
static void gc2_shutdown_restore_root_state(global_State *g, GCobj *o,
					     void *base)
{
  GCRootStateRef rootstate;
  int link = gc_root_link_claim_at(g, o, base, &rootstate);
  if (link == LJ_GC_ROOT_LINK_ALREADY)
    return;
  if (link != LJ_GC_ROOT_LINKED || !gc_root_link_commit(&rootstate)) {
    lj_assertG(0, "terminal root membership rollback failed");
    abort();
  }
}

static uint32_t gc2_shutdown_free_roots(global_State *g,
					 GCShutdownSeen *seen,
					 int *blockedp)
{
  GCobj *maino = obj2gco(mainthread_acq(g));
  GCRef *p = lj_gc_root_ref(g);
  GCobj *o;
  uint32_t freed = 0;
  if (blockedp)
    *blockedp = 0;
  gc_shutdown_seen_clear(seen);
  while ((o = gcref_acq(*p)) != NULL) {
    GCobj *next;
    GC2ShutdownDestroy sd;
    int seenrc = gc_shutdown_seen_add(seen, o);
    if (seenrc <= 0) {
      /* A duplicate intrusive node necessarily closes a cycle: one object has
      ** only one gcw link. Allocation failure is also fail-safe: stop before
      ** dereferencing an untracked body rather than risking terminal UAF. */
      lj_assertG(0, "terminal root spine duplicate or seen-set OOM");
      abort();
    }
    if (!gc_root_link_valid(g, o)) {
      lj_assertG(0, "invalid terminal root spine identity");
      abort();
    }
    next = lj_obj_gcw_acq(o);
    if (o == maino) {
      p = lj_obj_gcwref(o);
      continue;
    }
    if (LJ_UNLIKELY(!gc2_shutdown_destroy_init(g, o, &sd))) {
      lj_assertG(0, "invalid terminal GC destructor identity");
      abort();
    }
#if LJ_HASJIT
    if (sd.gct == (uint32_t)~LJ_TTRACE) {
      GCtrace *T = gco2trace(o);
      /* close_state's joined-world flush must have transferred every public
      ** trace to the token-owned retire list before the GC root drain begins.
      ** Make that prerequisite explicit: sfixed lj_trace_free_gc() otherwise
      ** permits an epoch-zero trace to take trace_free_immediate(), which would
      ** physically free `o` while p->o and its root side-plane are still live. */
      if (LJ_UNLIKELY(la_load64_acq(&T->retire_epoch) == 0 ||
	  !trace_retired_link_listed_acq(T))) {
	lj_assertG(0, "terminal root trace lacks retire-list ownership");
	abort();
      }
      if (!gc2_shutdown_reconcile_base(g, sd.base))
	abort();
      if (!lj_trace_free_gc(g, T)) {
	if (blockedp) *blockedp = 1;
	break;
      }
      /* Slot/debug teardown cannot consume the intrusive list owner. Recheck
      ** before releasing MEMBER and splicing the last root-spine edge. */
      if (LJ_UNLIKELY(la_load64_acq(&T->retire_epoch) == 0 ||
	  !trace_retired_link_listed_acq(T))) {
	lj_assertG(0, "terminal trace lost retire-list ownership");
	abort();
      }
      if (LJ_UNLIKELY(!gc2_shutdown_release_root_state(g, o))) {
	lj_assertG(0, "terminal trace root reconciliation failed");
	abort();
      }
    } else
#endif
    {
      /* Keep p->o intact while clearing the side-plane and acquiring physical
      ** lifetime. A losing nonblocking acquire is rolled back to MEMBER before
      ** returning, so no object disappears between terminal rounds. */
      if (LJ_UNLIKELY(!gc2_shutdown_release_root_state(g, o))) {
	lj_assertG(0, "terminal root membership reconciliation failed");
	abort();
      }
      if (!gc2_shutdown_destroy_acquire(g, &sd)) {
	gc2_shutdown_restore_root_state(g, o, sd.base);
	if (blockedp) *blockedp = 1;
	break;
      }
    }
    /* Only acquired lifetime or an exact trace retire-list owner permits the
    ** incoming edge to be spliced. Type-specific destruction runs afterward. */
    if (next)
      setgcrefrel(*p, next);
    else
      setgcrefnullrel(*p);
#if LJ_HASJIT
    if (sd.gct != (uint32_t)~LJ_TTRACE)
#endif
      gc2_shutdown_destroy_commit(g, o, &sd);
    freed++;
  }
  return freed;
}

static uint32_t gc2_shutdown_free_strings(global_State *g,
					  GCShutdownSeen *seen,
					  int *blockedp)
{
  StrTabHdr *hdr = lj_str_tabh_acq(g);
  MSize i;
  uint32_t freed = 0;
  if (blockedp)
    *blockedp = 0;
  if (hdr) {
    gc_shutdown_seen_clear(seen);
    for (i = hdr->mask; i != ~(MSize)0; i--) {
      GCRef *bucket = &hdr->bucket[i];
      uintptr_t u = lj_str_ref_load_acq(bucket);
      GCobj *o = (GCobj *)(u & ~(uintptr_t)LJ_STRHASH_LINKMASK);
      uintptr_t secondary = u & LJ_STRHASH_SECONDARY;
      while (o) {
	GCobj *next;
	LJGCDestructCtx dctx;
	int acquired;
	int seenrc = gc_shutdown_seen_add(seen, o);
	if (seenrc <= 0) {
	  lj_assertG(0, "terminal string chain duplicate or seen-set OOM");
	  abort();
	}
	if (la_load8_acq(&o->gch.gct) != (uint8_t)~LJ_TSTR) {
	  lj_assertG(0, "invalid terminal interned-string identity");
	  abort();
	}
	next = lj_str_next_acq(o);
	/* Keep the bucket edge until the exact gate and destructor lifetime LP
	** succeed. A loss leaves this string and its complete suffix discoverable
	** for the next PRE/retry round. */
	if (!gc2_shutdown_reconcile_base(g, o))
	  abort();
	acquired = lj_str_free_prepare(g, gco2str(o), &dctx);
	if (acquired == LJ_GC_DESTRUCT_LOST) {
	  if (blockedp) *blockedp = 1;
	  return freed;
	}
	/* The exact lifetime owner now prevents reuse. Publish the successor before
	** commit mutates the string type/count or releases its allocation bytes. */
	lj_str_ref_store_rel(bucket,
	  (uintptr_t)next | secondary);
	if (acquired == LJ_GC_DESTRUCT_ACQUIRED)
	  lj_str_free_commit(g, gco2str(o), &dctx);
	freed++;
	o = next;
      }
    }
  }
  /* Unlinked bodies are no longer discoverable from the active table. */
  lj_str_free_retired_bodies(g);
  return freed;
}

/* Free all remaining GC objects with GC2 ownership metadata only. This is a
** terminal, single-threaded drain after threading shutdown and finalizers; it
** deliberately never starts the retired color marker or sweeper. */
void lj_gc2_freeall(global_State *g)
{
  GCShutdownSeen seen;
  GCobj *maino;
  uint32_t stalled = 0;
  memset(&seen, 0, sizeof(seen));
  (void)lj_gc_repair_root_spine(g);
  for (;;) {
    uint32_t pre_ssb = lj_gc2_shutdown_discard_ssb(g);
    uint32_t pre_roots = lj_gc_flush_root_pending(g);
    uint32_t post_ssb, post_roots, freed;
    int blocked = 0;
    (void)lj_gc_repair_root_spine(g);
    if (!lj_gc2_terminal_prefree(g))
      abort();
    freed = gc2_shutdown_free_roots(g, &seen, &blocked);
    /* Thread/upvalue/userdata destruction can publish exact root or SSB work.
    ** Consume it before deciding whether this was a stable terminal round. */
    post_ssb = lj_gc2_shutdown_discard_ssb(g);
    post_roots = lj_gc_flush_root_pending(g);
    (void)lj_gc_repair_root_spine(g);
    if (!blocked && freed == 0 && pre_ssb == 0 && pre_roots == 0 &&
	post_ssb == 0 && post_roots == 0)
      break;
    if (blocked && freed == 0 && pre_ssb == 0 && pre_roots == 0 &&
	post_ssb == 0 && post_roots == 0) {
      /* One retry is required because the losing destructor itself may have
      ** published count-zero C|P. The next PRE consumes it. A second identical
      ** round has no remaining publisher and is an unresolved ownership bug. */
      if (++stalled >= 2u) {
	lj_assertG(0, "terminal root destructor made no progress");
	abort();
      }
    } else {
      stalled = 0;
    }
  }
  if (!lj_gc2_terminal_prefree(g))
    abort();
  maino = obj2gco(mainthread_acq(g));
  lj_obj_setgcwrel(maino, NULL);
  lj_gc_root_rel(g, maino);
  stalled = 0;
  for (;;) {
    uint32_t freed;
    int blocked = 0;
    if (!lj_gc2_terminal_prefree(g))
      abort();
    freed = gc2_shutdown_free_strings(g, &seen, &blocked);
    if (!blocked)
      break;
    if (freed == 0 && ++stalled >= 2u) {
      lj_assertG(0, "terminal string destructor made no progress");
      abort();
    }
    if (freed != 0)
      stalled = 0;
  }
  /* FINAL catches a C|P publication from the last type-specific destructor. */
  (void)lj_gc2_shutdown_discard_ssb(g);
  (void)lj_gc_flush_root_pending(g);
  (void)lj_gc_repair_root_spine(g);
  if (!lj_gc2_terminal_prefree(g))
    abort();
  gc_shutdown_seen_fini(&seen);
}

#if LJ_HASJIT
static int gc_jit_phase_threshold_exit_due(global_State *g)
{
  uint32_t phase;
  if (!lj_tg_any_jit_active(g))
    return 0;
  if (!lj_gc2_jit_entry_open(g))
    return 1;
  phase = gc2_phase_acq(g);
  /* An open SWEEP lease is already inside an active cycle. The ordinary debt
  ** threshold schedules work but is not a root/reclaim boundary; the hard
  ** cadence below closes the gate and forces authoritative interpreter
  ** progress. IDLE still uses its threshold to request/start a new cycle. */
  return phase != LJ_GC2_SWEEP &&
    lj_gc_total_load(g) >= lj_gc_threshold_load(g);
}

int lj_gc2_jit_needs_exit(global_State *g)
{
  return gc_jit_phase_threshold_exit_due(g) ||
	 (lj_tg_any_jit_active(g) && gc_hard_assist_due_jit(g));
}
#else
#define lj_gc2_jit_needs_exit(g)	0
#endif

static int gc2_step_auto(lua_State *L, int threshold_step, uint32_t step_limit)
{
  global_State *g = G(L);
  GCSize quantum = gc_step_debt_quantum(g);
  int32_t ostate = vmstate_load_acq(g);
  uint64_t sweep_arenas0 = gc2_sweep_owner_arenas_acq(g);
  int running = gc_logical_running(g);
  int drove = 0;
  int done = 0;
  int mark_dispatch_yield = 0;
  if (step_limit == 0)
    step_limit = 1;
  setvmstate(g, GC);
  if (running)
    lj_gc2_check_trigger(g, L2TG(L));
  while (running && step_limit-- != 0) {
    uint64_t defer0 = gc2_deferred_epoch_acq(g);
    uint32_t phase = gc2_phase_acq(g);
    int sweep_step = phase == LJ_GC2_SWEEP;
    if (phase == LJ_GC2_IDLE && gc2_cycle_leader_acq(g) == 0 &&
	!threshold_step)
      break;
    drove = 1;
    done = lj_gc2_step_explicit(L, 1);
    /* A retained owner ends this automatic batch as well as the inner step. */
    if (gc2_deferred_epoch_acq(g) != defer0)
      break;
    if (gc2_phase_acq(g) == LJ_GC2_IDLE)
      break;
    /* Activation grants a bounded number of pre-dispatch opportunities. Each
    ** miss still completes one collector unit, a real x64 native entry clears
    ** the remaining allowance, and exhaustion falls back to the full automatic
    ** batch. This reaches JLOOP without allowing interpreter allocation to
    ** postpone MARK indefinitely. */
    if (gc2_phase_acq(g) == LJ_GC2_MARK && lj_gc2_jit_entry_open(g) &&
	gc2_jit_mark_auto_yield_take(g)) {
      mark_dispatch_yield = 1;
      break;
    }
    /* Keep driving grace/classification units, but stop after one complete
    ** LJ_GC2_SWEEP_BATCH of arenas. Without this aggregate bound, each of the
    ** GCACTIVEAUTOSTEPS units could itself complete a full arena batch. */
    if (sweep_step &&
	gc2_sweep_owner_arenas_acq(g) - sweep_arenas0 >= LJ_GC2_SWEEP_BATCH)
      break;
  }
  g->gc.debt = 0;
  if (gc2_phase_acq(g) == LJ_GC2_IDLE) {
    if (running)
      lj_gc2_publish_idle_threshold(g);
    vmstate_store_rel(g, ostate);
    return done ? 1 : (drove ? 0 : -1);
  }
  /* A pre-dispatch MARK lease is not permission to move the allocation
  ** threshold. Leave it due so the first subsequent allocation—traced or
  ** interpreted—must pay the next bounded collector quantum. This makes the
  ** native opportunity a one-dispatch handoff instead of unbounded debt
  ** deferral until the next accounting quantum. */
  lj_gc_threshold_store(g, lj_gc_total_load(g) +
	(mark_dispatch_yield ? 0 : quantum));
  vmstate_store_rel(g, ostate);
  return -1;
}

/* Perform a limited amount of incremental GC steps. */
int LJ_FASTCALL lj_gc_step(lua_State *L)
{
  global_State *g = G(L);
  return gc2_step_auto(L, lj_gc_total_load(g) >= lj_gc_threshold_load(g),
		       GCACTIVEAUTOSTEPS);
}

#ifdef LJ_GC2_TEST_HELPERS
static uint32_t gc_test_step_fixtop_calls;
static LJGcRootPendingLoadHook gc_test_root_pending_load_hook;
static LJGcRootStateHook gc_test_root_state_hook;

void lj_gc_test_set_root_pending_load_hook(LJGcRootPendingLoadHook hook)
{
  la_storeptr_rel((void **)&gc_test_root_pending_load_hook, (void *)hook);
}

void lj_gc_test_set_root_state_hook(LJGcRootStateHook hook)
{
  la_storeptr_rel((void **)&gc_test_root_state_hook, (void *)hook);
}

int lj_gc_test_unlink_root_obj_bounded(global_State *g, GCobj *dead,
					uint32_t limit)
{
  return gc_unlink_root_obj_mode(g, dead, limit, 0);
}

static void gc_test_root_state(global_State *g, GCobj *o, uint32_t path)
{
  LJGcRootStateHook hook = (LJGcRootStateHook)
    la_loadptr_acq((void *const *)&gc_test_root_state_hook);
  if (hook)
    hook(g, o, path);
}

static LJ_AINLINE void gc_test_root_pending_loaded(global_State *g,
					    TGState *tg, GCobj *published,
					    GCobj *observed, uint32_t path)
{
  LJGcRootPendingLoadHook hook = (LJGcRootPendingLoadHook)
    la_loadptr_acq((void *const *)&gc_test_root_pending_load_hook);
  if (hook)
    hook(g, tg, published, observed, path);
}

GCobj *lj_gc_test_root_pending_loaded_vm(global_State *g, TGState *tg,
					 GCobj *published, GCobj *observed)
{
  gc_test_root_pending_loaded(g, tg, published, observed,
			      LJ_GC_ROOT_PENDING_TEST_VM_TNEW);
  return published;
}

static LJ_AINLINE void gc_test_step_fixtop_call(void)
{
  (void)la_add32_acqrel(&gc_test_step_fixtop_calls, 1);
}

uint32_t lj_gc_test_step_fixtop_calls(void)
{
  return la_load32_acq(&gc_test_step_fixtop_calls);
}

void lj_gc_test_reset_step_fixtop_calls(void)
{
  la_store32_rel(&gc_test_step_fixtop_calls, 0);
}
#else
#define gc_test_step_fixtop_call()	((void)0)
#define gc_test_root_pending_loaded(g, tg, published, observed, path) \
  ((void)0)
#endif

static void gc_step_assist_top(lua_State *L, global_State *g, int threshold_step)
{
  TGState *tg = L2TG(L);
  lj_gc2_check_trigger(g, L2TG(L));
  if (!threshold_step)
    threshold_step = lj_gc_total_load(g) >= lj_gc_threshold_load(g);
  if (gc_hard_assist_due_interp(g, tg)) {
    gc2_interp_hard_checks_add(g, 1);
    lj_gc2_assist(g, tg);  /* 05 section 5.11 interpreter assist bridge. */
  }
  if (threshold_step || lj_gc_pending_auto_request(g))
    gc2_step_auto(L, threshold_step, GCACTIVEAUTOSTEPS);
}

/* Ditto, but fix the stack top first. */
void LJ_FASTCALL lj_gc_step_fixtop(lua_State *L)
{
  global_State *g = G(L);
  gc_test_step_fixtop_call();
  if (curr_funcisL(L)) L->top = curr_topL(L);
  gc_step_assist_top(L, g, lj_gc_total_load(g) >= lj_gc_threshold_load(g));
}

/* Ditto, but use an already fixed stack top. */
void LJ_FASTCALL lj_gc_step_top(lua_State *L)
{
  global_State *g = G(L);
  gc_step_assist_top(L, g, lj_gc_total_load(g) >= lj_gc_threshold_load(g));
}

#if LJ_HASJIT
/* Perform multiple GC steps. Called from JIT-compiled code. */
int LJ_FASTCALL lj_gc_step_jit(global_State *g, MSize steps)
{
  lua_State *L = lj_tg_cur_L(g);
  TValue *jbase = lj_tg_jit_base(g);
  int threshold_step, hard_step, needs_exit;
  TGState *tg;
  if (!L || !jbase)
    return 1;
  L->base = jbase;
  L->top = curr_topL(L);
  tg = L2TG(L);
  /* Split an active/threshold exit from a hard-only trace cadence check. The
  ** former must not run trace-owned phase progress. The latter must advance
  ** hard_check_bytes without accidentally starting a stopped-IDLE cycle. */
  needs_exit = gc_jit_phase_threshold_exit_due(g);
  hard_step = gc_hard_assist_due_jit(g);
  if (!needs_exit && !hard_step)
    lj_gc2_check_trigger(g, tg);
  threshold_step = lj_gc_total_load(g) >= lj_gc_threshold_load(g);
  hard_step = gc_hard_assist_due_jit(g);
  if (hard_step) {
    /* A phase/threshold exit owns progress from the interpreter. Service the
    ** trace cadence and telemetry, but do not run assist work before leaving. */
    needs_exit = needs_exit || gc_jit_phase_threshold_exit_due(g);
    gc2_jit_hard_checks_add(g, 1);
    if (!needs_exit)
      lj_gc2_assist(g, tg);  /* 05 section 5.11 trace-side assist bridge. */
    /* MARK assist is logical and bounded. Close native admission only after it
    ** releases collector ownership, then advance the hard cadence so this
    ** exact trace must restore its snapshot before fixpoint work can begin. */
    if (!needs_exit && gc2_phase_acq(g) == LJ_GC2_MARK &&
	lj_gc2_jit_entry_open(g)) {
      lj_gc2_jit_mark_request_exit(g);
      needs_exit = 1;
    }
    lj_gc2_hard_check_advance(g, lj_gc2_alloc_since_load(g));
    /* SWEEP assist is intentionally a no-op. Its hard cadence is instead the
    ** single-TG/no-worker progress backstop: close entry asynchronously and
    ** let this exact trace restore its snapshot before the interpreter owns a
    ** bounded reclaim quantum. */
    if (!needs_exit && gc2_phase_acq(g) == LJ_GC2_SWEEP &&
	lj_gc2_jit_entry_open(g)) {
      lj_gc2_jit_sweep_request_exit(g);
      needs_exit = 1;
    }
  }
  /* hard_check_bytes may now be beyond current debt. Keep executing a stopped
  ** IDLE trace in that case; preserve a pre-sampled or newly-due phase/threshold
  ** exit otherwise. */
  needs_exit = needs_exit || lj_gc2_jit_needs_exit(g);
  if (needs_exit && gc2_phase_acq(g) == LJ_GC2_MARK &&
      lj_gc2_jit_entry_open(g))
    lj_gc2_jit_mark_request_exit(g);
  if (needs_exit)
    return 1;
  if (threshold_step && gc2_phase_acq(g) == LJ_GC2_SWEEP &&
      lj_gc2_jit_entry_open(g)) {
    /* Allocation is already governed by the post-RESET SWEEP policy: major
    ** births are black, while minor births use fresh non-sweep generations.
    ** Phase-aware root/store barriers publish old-generation rescues. Merely
    ** move the active-cycle scheduling threshold; SWEEP->IDLE recomputes final
    ** pacing from live bytes and total allocation. */
    lj_gc_threshold_store(g,
	lj_gc_total_load(g) + gc_step_debt_quantum(g));
    if (gc2_n_workers_acq(g) != 0)
      lj_gc2_sweep_publish_wake(g);
    threshold_step = 0;
  }
  if (threshold_step) {
    while (steps-- > 0 && gc2_step_auto(L, threshold_step, 1) == 0)
      threshold_step = lj_gc_total_load(g) >= lj_gc_threshold_load(g);
  }
  needs_exit = lj_gc2_jit_needs_exit(g);
  /*
  ** Return 1 to force a trace exit when GC2 needs interpreter-owned progress:
  ** mark root snapshots, weak/sweep transitions, or finalizer dispatch. GC2
  ** active-black allocation can make a trace-local container live before its
  ** payload has been closed by a root scan; leaving at the poll gives the
  ** interpreter-side scanner an authoritative frame before the cycle advances.
  */
  return needs_exit;
}
#endif

/* -- Write barriers ------------------------------------------------------ */

/* Barrier for a store to a global root slot. */
void lj_gc_pubroot(lua_State *L, cTValue *tv)
{
  global_State *g;
  uint32_t phase;
  TValue snap;
  if (!L || !tv)
    return;
  g = G(L);
  if (LJ_UNLIKELY(g == NULL && L->tg_hint != NULL && L->tg_hint->gl != NULL)) {
    g = L->tg_hint->gl;
    setmref(L->glref, g);
  }
  if (LJ_UNLIKELY(g == NULL))
    return;
  phase = gc2_phase_acq(g);
  /* Incremental IDLE has neither a current mark frontier nor an old-to-young
  ** remembered set. A cycle starting after this acquire observes the value's
  ** already-published source, or its caller publication before the mutator
  ** acknowledges activation, so avoid copying/tagging this exact no-op case. */
  if (phase == LJ_GC2_IDLE && !gc2_generational_acq(g))
    return;
  lj_tv_load_acq(&snap, tv);
  if (tvisgcv(&snap)) {
    /* GC2 is the sole runtime collector. Its publication barrier covers
    ** MARK/WEAK, SWEEP resurrection and the generational IDLE remembered set;
    ** never revive or traverse the retired color collector here. */
    lj_gc2_barrier_tv_g(g, &snap);
  }
}

/* Publish a GC object that is becoming reachable from a native root list. */
void lj_gc_pubobjroot(lua_State *L, GCobj *o)
{
  global_State *g;
  if (!L || !o)
    return;
  /* The object barrier retains before inspecting the header and routes SWEEP
  ** roots directly through rescue. Synthesizing a TValue tag here would first
  ** require the unsafe header read this publication is meant to protect. */
  g = G(L);
  if (gc2_phase_acq(g) != LJ_GC2_IDLE)
    lj_gc2_barrier_obj_pair_g(g, NULL, o);
  else
    lj_gc2_barrier_obj_pair(L, NULL, o);
}

/* Compatibility entry for a published object edge. GC2 owns the runtime
** frontier; the legacy color frontier is never entered. */
void lj_gc_barrierf(global_State *g, GCobj *o, GCobj *v)
{
  if (!g || !v)
    return;
  lj_gc2_barrier_obj_pair_g(g, o, v);
}

/* VM-callable table black-to-gray repair. */
void lj_gc_barrierback_tab_g(global_State *g, GCtab *t)
{
  if (g && t)
    lj_gc2_barrier_tab_g(g, t);
}

void lj_gc_tbar_trace_g(global_State *g, GCtab *t, cTValue *key)
{
  if (!g || !t)
    return;
  if (key)
    lj_gc2_barrier_key_g(g, t, key);
  else
    lj_gc2_barrier_tab_g(g, t);
}

void lj_gc_pubtabkey_(lua_State *L, GCtab *t, cTValue *key)
{
  global_State *g;
  TGState *tg;
  if (!L || !t || !key)
    return;
  g = G(L);
  tg = L2TG(L);
  /* A non-generational IDLE key barrier has no GC2 work. If MARK starts after
  ** this sample, the already-published table slot precedes this TG's activation
  ** acknowledgement and is covered by its root snapshot. */
  if (tg && !lj_tg_mark_active_acq(tg) &&
      gc2_phase_acq(g) == LJ_GC2_IDLE && gc2_generational_acq(g) == 0)
    return;
  if (LJ_UNLIKELY(!lj_gc_tv_gcref_valid(g, key)))
    return;
  lj_gc2_barrier_key_g(g, t, key);
}

/* Publication wrapper for x64 VM table -> object stores. */
void lj_gc_pubtabobj_vm(lua_State *L, GCtab *t, GCobj *o)
{
  if (!L || !t || !o)
    return;
  lj_gc2_barrier_obj_pair(L, obj2gco(t), o);
}

/* Publication wrapper for x64 VM table -> TValue stores. */
void lj_gc_pubtabtv_vm(lua_State *L, GCtab *t, cTValue *tv)
{
  if (!L || !t || !tv)
    return;
  lj_gc_barriertv_(L, t, tv);
}

/* Publication wrapper for x64 VM table range stores. */
void lj_gc_pubtabtvn_vm(lua_State *L, GCtab *t, cTValue *tv, uint32_t n)
{
  if (!L || !t || !tv || n == 0)
    return;
  lj_gc2_barrier_tvn_pair_g(G(L), obj2gco(t), tv, n);
  lj_gc2_barrier_tab(L, t);  /* Preserve the previous TSETM table barrier. */
}

/* Publication wrapper for x64 VM table -> stack loads. */
void lj_gc_pubtvroot_vm(lua_State *L, cTValue *tv)
{
  if (!L || !tv)
    return;
  /* DynASM stores and dirties the slot before entering this cold wrapper. */
  lj_gc_pubroot(L, tv);
}

/* Publication wrapper for closed-upvalue TValue stores. Pass &uv->tv. */
void LJ_FASTCALL lj_gc_pubuv(global_State *g, TValue *tv)
{
  GCupval *uv = (GCupval *)((char *)tv - offsetof(GCupval, tv));
  TValue snap;
  lj_tv_load_acq(&snap, tv);
  if (!tvisgcv(&snap))
    return;
  lj_gc2_barrier_tv_pair_g(g, obj2gco(uv), &snap);
}

/* Close upvalue. Also needs a write barrier. */
void lj_gc_closeuv(global_State *g, GCupval *uv)
{
  GCobj *o = obj2gco(uv);
  int link;
  /* Copy stack slot to upvalue itself and point to the copy. */
  copyTVrel(mainthread_acq(g), &uv->tv, uvval(uv));
  setmref(uv->v, &uv->tv);
  uv->closed = 1;
  /*
  ** lj_func_closeuv() has already removed the upvalue from the per-state open
  ** chain, so its nextgc link is available for object-list
  ** publication. Liveness is still discovered through closures that reference
  ** the GCupval; the object list is the sweep/free spine and every consumer of
  ** that spine flushes pending roots first. Queueing here avoids a global
  ** root-list CAS on the close-upvalue path without changing collector reach.
  */
  link = lj_gc_linkobj_pending(g, o);
  if (link <= LJ_GC_ROOT_LINK_DEFER)
    /* A concurrent unlink/link owner keeps gcw private. Preserve the closed
    ** upvalue in the semantic frontier and let the later bounded owner retry;
    ** closure edges remain the authoritative reachability relation. */
    (void)lj_gc2_markmem(g, o);
  lj_gc2_barrier_tv_pair_g(g, o, &uv->tv);
}

#if LJ_HASJIT
/* Mark a trace if it's saved during the propagation phase. */
void lj_gc_pubtrace(global_State *g, uint32_t traceno)
{
  if (gc2_phase_acq(g) != LJ_GC2_IDLE)
    lj_gc2_mark_trace_slot(g, traceno);
}
#endif

/* -- Allocator ----------------------------------------------------------- */

static LJArenaAllocD *gc_arena_allocd_for_tg(global_State *g, TGState *tg)
{
  if (tg && lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL))
    return &tg->allocd;
  return (LJArenaAllocD *)g->allocd;
}

static LJArenaAllocD *gc_arena_allocd_for_new(lua_State *L)
{
  return gc_arena_allocd_for_tg(G(L), L2TG(L));
}

static LJArenaAllocD *gc_arena_allocd_for_ptr(global_State *g, const void *p)
{
  if (p) {
    uint32_t owner_tid = lj_arena_owner_acq(lj_arena_of(p));
    TGState *tg = lj_thr_get_tg();
    /* Constructors normally publish into their own arena. Keep that hot path
    ** O(1): the exact TLS binding pins tg, while the acquired arena owner id
    ** rejects a concurrently transferred arena. The main TG is state-lifetime
    ** stable and covers owner-side publication after worker adoption. */
    if (tg && tg->gl == g && lj_tg_tid_acq(tg) == owner_tid &&
	lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL))
      return &tg->allocd;
    tg = g->main_tg;
    if (tg && lj_tg_tid_acq(tg) == owner_tid &&
	lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL))
      return &tg->allocd;
    tg = lj_tg_find_owner(g, owner_tid);
    if (tg)
      return gc_arena_allocd_for_tg(g, tg);
  }
  return (LJArenaAllocD *)g->allocd;
}

static int gc_destructor_enter_impl(global_State *g, void *base, GCSize size,
				    LJGCDestructCtx *ctx,
				    int reclaim_held)
{
  GCArena *a;
  if (ctx)
    memset(ctx, 0, sizeof(*ctx));
  if (!g || !base || size == 0 || !ctx)
    return LJ_GC_DESTRUCT_LOST;
  ctx->base = base;
  ctx->size = size;
  if (g->allocf != lj_arena_allocf ||
      la_load32_acq(&g->allocf_arena) == 0)
    return LJ_GC_DESTRUCT_ACQUIRED;
  if (!checkptrGC(base) ||
      !(reclaim_held ?
	lj_gc2_mem_registered_known_reclaim_held(g, base) :
	lj_gc2_mem_registered_known(g, base)))
    return LJ_GC_DESTRUCT_LOST;
  a = lj_arena_of(base);
  if (!lj_arena_ishuge(a)) {
    int result = lj_arena_destruct_acquire(base, size);
    return result == LJ_ARENA_DESTRUCT_ACQUIRED ?
	   LJ_GC_DESTRUCT_ACQUIRED :
	   result == LJ_ARENA_DESTRUCT_OWNED ?
	   LJ_GC_DESTRUCT_OWNED : LJ_GC_DESTRUCT_LOST;
  } else {
    LJArenaAllocD *ad = gc_arena_allocd_for_ptr(g, base);
    LJHugeInfo hi;
    int result;
    if (!ad || !ad->huge)
      return LJ_GC_DESTRUCT_LOST;
    result = lj_arena_hugetab_destruct_acquire(ad->huge, base, &hi);
    if (result == LJ_ARENA_DESTRUCT_ACQUIRED) {
      ctx->hugetab = ad->huge;
      ctx->huge_claim = 1;
      return LJ_GC_DESTRUCT_ACQUIRED;
    }
    if (result == LJ_ARENA_DESTRUCT_OWNED)
      return LJ_GC_DESTRUCT_OWNED;
    return LJ_GC_DESTRUCT_LOST;
  }
}

int lj_gc_destructor_enter(global_State *g, void *base, GCSize size,
			    LJGCDestructCtx *ctx)
{
  return gc_destructor_enter_impl(g, base, size, ctx, 0);
}

int lj_gc_destructor_enter_reclaim_held(global_State *g, void *base,
					 GCSize size, LJGCDestructCtx *ctx)
{
  return gc_destructor_enter_impl(g, base, size, ctx, 1);
}

void lj_gc_destructor_leave(global_State *g, LJGCDestructCtx *ctx)
{
  if (ctx && ctx->huge_claim) {
    HugeTab *ht = (HugeTab *)ctx->hugetab;
    LJHugeInfo hi;
    int finish = lj_arena_hugetab_finish_external_free(
	      ht, ctx->base, &hi);
    if (finish == LJ_ARENA_HUGE_FINISH_UNMAP) {
      lj_arena_huge_unmap_claimed(ctx->base, hi.size);
    } else if (finish == LJ_ARENA_HUGE_FINISH_DEFERRED) {
      gc2_sweep_grace_needed_rel(g, 1);
      lj_gc2_sweep_publish_wake(g);
    } else {
      lj_assertG(0, "huge GC destructor ownership lost");
      abort();
    }
    ctx->huge_claim = 0;
  }
}

static int gc_arena_ptr_nonreallocable(LJArenaAllocD *ad, const void *p)
{
  GCArena *a;
  if (!ad || !p)
    return 0;
  a = lj_arena_of(p);
  if (!lj_arena_ishuge(a))
    return (lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE) != 0;
  if (ad->huge) {
    LJHugeInfo hi;
    if (lj_arena_hugetab_lookup(ad->huge, p, &hi) == 1)
      return (hi.flags & LJ_HUGEF_TRAVERSABLE) != 0;
  }
  return 0;
}

static void *gc_mem_new_nothrow(lua_State *L, GCSize size, int account)
{
  global_State *g = G(L);
  void *p;
  lj_assertG(size != 0, "zero-sized non-throwing allocation");
  if (g->allocf == lj_arena_allocf) {
    LJArenaAllocD *ad = gc_arena_allocd_for_new(L);
    p = lj_arena_allocf(ad, NULL, 0, size);
  } else {
    p = g->allocf(g->allocd, NULL, 0, size);
  }
  if (p == NULL)
    return NULL;
  lj_assertG(checkptrGC(p),
	     "allocated memory address %p outside required range", p);
  lj_gc_total_add(g, size);
  if (account)
    lj_gc2_account_alloc(g, L2TG(L), size);  /* 04 section 4.8. */
  return p;
}

/* Allocate a new fragment without raising an error on allocation failure. */
void *lj_mem_new_nothrow(lua_State *L, GCSize size)
{
  return gc_mem_new_nothrow(L, size, 1);
}

void *lj_mem_new_deferred_nothrow(lua_State *L, GCSize size)
{
  return gc_mem_new_nothrow(L, size, 0);
}

void lj_mem_account_deferred(lua_State *L, GCSize size)
{
  if (L && size != 0)
    lj_gc2_account_alloc(G(L), L2TG(L), size);
}

/* Call pluggable memory allocator to allocate or resize a fragment. */
void *lj_mem_realloc(lua_State *L, void *p, GCSize osz, GCSize nsz)
{
  global_State *g = G(L);
  lj_assertG((osz == 0) == (p == NULL), "realloc API violation");
  if (g->allocf == lj_arena_allocf) {
    LJArenaAllocD *ad = p ? gc_arena_allocd_for_ptr(g, p) :
			    gc_arena_allocd_for_new(L);
    /* GC headers and unpublished constructor bodies have identity metadata
    ** tied to their exact allocation base and extent. They are deliberately
    ** non-reallocable; type-specific teardown must use lj_mem_free() after the
    ** object is disconnected. Silently moving one would transfer READY/cdata
    ** state onto bytes with no valid publication history. */
    if (p && gc_arena_ptr_nonreallocable(ad, p)) {
      lj_assertG(0, "attempt to resize traversable GC allocation");
      abort();
    }
    p = lj_arena_allocf(ad, p, osz, nsz);
  } else {
    p = g->allocf(g->allocd, p, osz, nsz);
  }
  if (p == NULL && nsz > 0)
    lj_err_mem(L);
  lj_assertG((nsz == 0) == (p == NULL), "allocf API violation");
  lj_assertG(checkptrGC(p),
	     "allocated memory address %p outside required range", p);
  lj_gc_total_adjust(g, osz, nsz);
  if (nsz > osz)
    lj_gc2_account_alloc(g, L2TG(L), nsz - osz);  /* 04 section 4.8. */
  return p;
}

static void *gc_mem_newgco_raw_nothrow(lua_State *L, GCSize size,
					uint32_t flags, int account)
{
  global_State *g = G(L);
  GCobj *o;
  if (g->allocf == lj_arena_allocf)
    o = (GCobj *)lj_arena_allocd_alloc(gc_arena_allocd_for_new(L), size,
				       flags);
  else
    o = (GCobj *)g->allocf(g->allocd, NULL, 0, size);
  if (o == NULL)
    return NULL;
  lj_assertG(checkptrGC(o),
	     "allocated memory address %p outside required range", o);
  lj_gc_total_add(g, size);
  if (account)
    lj_gc2_account_alloc(g, L2TG(L), size);  /* 04 section 4.8. */
  return o;
}

/* Allocate raw storage for a GC object without linking or throwing. */
void *lj_mem_newgco_raw_nothrow(lua_State *L, GCSize size, uint32_t flags)
{
  return gc_mem_newgco_raw_nothrow(L, size, flags, 1);
}

/* Allocate raw storage for a GC object without linking it. */
void *lj_mem_newgco_raw(lua_State *L, GCSize size, uint32_t flags)
{
  void *o = lj_mem_newgco_raw_nothrow(L, size, flags);
  if (o == NULL)
    lj_err_mem(L);
  return o;
}

int lj_mem_publish_cdata(lua_State *L, void *base, GCSize size, int interior)
{
  global_State *g = G(L);
  if (g->allocf != lj_arena_allocf)
    return 1;  /* Temporary custom-lua_Alloc compatibility path. */
  return lj_arena_allocd_publish_cdata(
	gc_arena_allocd_for_new(L), base, (size_t)size, interior);
}

int lj_mem_publish_interior_cdata(lua_State *L, void *base, GCSize size)
{
  return lj_mem_publish_cdata(L, base, size, 1);
}

void lj_gc_publishobj_header(global_State *g, GCobj *o)
{
  LJArenaAllocD *ad;
  void *route = o;
  int is_cdata = 0;
  if (!g || !o || g->allocf != lj_arena_allocf)
    return;
#if LJ_HASFFI
  if (la_load8_acq(&o->gch.gct) == (uint8_t)~LJ_TCDATA) {
    is_cdata = 1;
    if (cdataisv(gco2cd(o)))
      route = memcdatav(gco2cd(o));
  }
#endif
  ad = gc_arena_allocd_for_ptr(g, route);
  if (!is_cdata && route == (void *)o && ad && g->main_tg &&
      ad == &g->main_tg->allocd && lj_thr_get_tg() == g->main_tg &&
      mt_active_acq(g) == 0 && mt_entering_acq(g) == 0 &&
      gc2_n_workers_acq(g) == 0) {
    GCArena *a = lj_arena_of(o);
    if (!lj_arena_ishuge(a)) {
      uint32_t cell = lj_arena_cellof(o);
      if (cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS &&
	  (lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE) &&
	  lj_arena_bm_get(a->block, cell)) {
	/* The sole VM/allocator thread is also the sole READY writer here. */
	lj_arena_ready_set_exclusive(a, cell);
	return;
      }
    }
  }
  if (!is_cdata && route == (void *)o && lj_arena_allocd_publish_gco(ad, o))
    return;
  if (is_cdata) {
    GCArena *a = lj_arena_of(route);
    if (!lj_arena_ishuge(a)) {
      uint32_t cell = lj_arena_cellof(route);
      if (cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS &&
	  (lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE) &&
	  lj_arena_bm_get(a->block, cell) &&
	  lj_arena_cdata_get(a, cell) && lj_arena_ready_get(a, cell))
	return;
    } else if (ad && ad->huge) {
      LJHugeInfo hi;
      if (lj_arena_hugetab_lookup(ad->huge, route, &hi) == 1 &&
	  (hi.flags & (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_CDATA|
		       LJ_HUGEF_READY)) ==
	    (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_CDATA|LJ_HUGEF_READY) &&
	  ((route != (void *)o) ==
	   ((hi.flags & LJ_HUGEF_INTERIOR_CDATA) != 0)) &&
	  !(hi.flags & LJ_HUGEF_FREEING))
	return;
    }
  }
  /* Linking an undiscoverable header would let root traversal inspect partial
  ** bytes and would make constructor failure indistinguishable from a live
  ** object. This is a terminal internal invariant in every build. */
  lj_assertG(0, "GC object linked before header-ready publication");
  abort();
}

/* Fresh-object publication retains the allocator-issued base. Fixed-layout
** objects pass o; variable cdata passes its over-allocation base. This avoids
** both mutable shape recovery and process-wide arena discovery in the
** constructor's correctness-critical path. */
static int gc_publishobj_header_at(global_State *g, GCobj *o, void *base)
{
  LJArenaAllocD *ad;
  GCArena *a;
  int is_cdata = 0;
  if (!g || !o || !base)
    return 0;
  if (g->allocf != lj_arena_allocf)
    return 1;
#if LJ_HASFFI
  if (la_load8_acq(&o->gch.gct) == (uint8_t)~LJ_TCDATA) {
    is_cdata = 1;
    if (cdataisv(gco2cd(o)) ? memcdatav(gco2cd(o)) != base :
			      (void *)o != base)
      return 0;
  } else
#endif
  if ((void *)o != base)
    return 0;
  a = lj_arena_of(base);
  if (!lj_arena_ishuge(a)) {
    uint32_t cell = lj_arena_cellof(base);
    if (!is_cdata)
      return lj_arena_publish_gco_at(base);
    return cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS &&
      lj_arena_cellptr(a, cell) == base &&
      (lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE) != 0 &&
      lj_arena_bm_get(a->block, cell) && lj_arena_cdata_get(a, cell) &&
      lj_arena_ready_get(a, cell);
  }
  ad = gc_constructor_allocd_at(g, base);
  if (!ad || !ad->huge)
    return 0;
  if (!is_cdata)
    return lj_arena_hugetab_publish_gco(ad->huge, base);
  {
    LJHugeInfo hi;
    return lj_arena_hugetab_lookup(ad->huge, base, &hi) == 1 &&
      (hi.flags & (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_CDATA|
		   LJ_HUGEF_READY)) ==
        (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_CDATA|LJ_HUGEF_READY) &&
      ((base != (void *)o) ==
	((hi.flags & LJ_HUGEF_INTERIOR_CDATA) != 0)) &&
      !(hi.flags & LJ_HUGEF_FREEING);
  }
}

void *lj_mem_newgco_unlinked_nothrow(lua_State *L, GCSize size)
{
  return lj_mem_newgco_raw_nothrow(L, size,
				    LJ_AF_TRAVERSABLE|LJ_AF_ROOT_CONSTRUCT);
}

void *lj_mem_newgco_unlinked_deferred_nothrow(lua_State *L, GCSize size)
{
  return gc_mem_newgco_raw_nothrow(
	L, size, LJ_AF_TRAVERSABLE|LJ_AF_ROOT_CONSTRUCT, 0);
}

void *lj_mem_newgco_unlinked(lua_State *L, GCSize size)
{
  return lj_mem_newgco_raw(L, size,
			    LJ_AF_TRAVERSABLE|LJ_AF_ROOT_CONSTRUCT);
}

static int gc_root_publish_claimed(global_State *g, GCobj *o,
				    GCRootStateRef *rootstate)
{
  GCRef *root = lj_gc_root_ref(g);
  GCobj *head;
  do {
    head = gcref_acq(*root);
    if (head)
      lj_obj_setgcw(o, head);
    else
      lj_obj_setgcwnull(o);
  } while (!gcref_cas(root, &head, o));  /* M7 publish. */
  if (LJ_UNLIKELY(!gc_root_link_commit(rootstate))) {
    /* The exact object is already visible and must never be published again.
    ** Leaving LINKING is fail-closed for arena reuse/reclaim. */
    lj_assertG(0, "root membership link commit lost");
    return LJ_GC_ROOT_LINK_DEFER;
  }
  lj_gcroot_repair_epoch_add(g);
  return LJ_GC_ROOT_LINKED;
}

static int gc_root_publish_constructed(global_State *g, GCobj *o,
				       GCRootStateRef *rootstate)
{
  GCRef *root = lj_gc_root_ref(g);
  GCobj *head;
  do {
    head = gcref_acq(*root);
    if (head)
      lj_obj_setgcwrel(o, head);
    else
      lj_obj_setgcwnullrel(o);
  } while (!gcref_cas(root, &head, o));
  if (LJ_UNLIKELY(!gc_root_construct_commit(rootstate))) {
    /* Visibility is irreversible. Retaining LINKING/CONSTRUCT is the safe
    ** response to an impossible lost owner transition: reuse stays vetoed. */
    lj_assertG(0, "constructed root membership commit lost");
    abort();
  }
  lj_gcroot_repair_epoch_add(g);
  return LJ_GC_ROOT_LINKED;
}

int lj_gc_linkobj_at(global_State *g, GCobj *o, void *base)
{
  GCRootStateRef rootstate;
  int claim;
  if (!g || !o || !base)
    return LJ_GC_ROOT_LINK_INVALID;
  /* The caller's FINREG/ticket/terminal token resolved base without granting
  ** this publisher descriptor ownership. Small validation deliberately starts
  ** only after the following LIVE->MUTATING CAS succeeds. */
  claim = gc_root_link_claim_at(g, o, base, &rootstate);
  if (claim != LJ_GC_ROOT_LINKED)
    return claim;
  return gc_root_publish_claimed(g, o, &rootstate);
}

int lj_gc_linkobj(global_State *g, GCobj *o)
{
  /* Fixed-layout ordinary objects have allocation-base identity. Interior
  ** cdata dispatch must use lj_gc_linkobj_at() with its retained exact base. */
  return lj_gc_linkobj_at(g, o, o);
}

static LJ_AINLINE void gc_root_set_next(GCobj *o, GCobj *next)
{
  if (next)
    lj_obj_setgcw(o, next);
  else
    lj_obj_setgcwnull(o);
}

static LJ_AINLINE void gc_root_set_next_rel(GCobj *o, const GCobj *next)
{
  if (next)
    lj_obj_setgcwrel(o, next);
  else
    lj_obj_setgcwnullrel(o);
}

static uint32_t gc_root_chain_tail(global_State *g, GCobj *head, GCobj **tailp)
{
  GCobj *tail, *next, *tortoise, *hare;
  void *known_arena;
  uint32_t n, power, lam;
  if (!head) {
    *tailp = NULL;
    return 0;
  }
restart:
  /* Pending-root flush owns one SMR scope across detach, validation and
  ** splice. Brent's detector produces the healthy tail/count in that same
  ** single pass; only the exceptional cyclic case invokes the repair walk. */
  known_arena = NULL;
  n = 1;
  if (LJ_UNLIKELY(!gc_root_link_valid_held(g, head, &known_arena))) {
      /* head has already been xchg-detached from its per-TG pending slot. A
      ** silent short count would splice a valid prefix and lose this node plus
      ** its complete suffix. Registry lifetime is held across this walk, so a
      ** rejection is structural corruption, not retryable admission. */
      lj_assertG(0, "invalid detached pending-root node");
      abort();
  }
  tail = head;
  next = lj_obj_gcw_acq(head);
  if (!next)
    goto done;
  tortoise = head;
  hare = next;
  power = lam = 1;
  while (tortoise != hare) {
    if (LJ_UNLIKELY(!gc_root_link_valid_held(g, hare, &known_arena))) {
      lj_assertG(0, "invalid detached pending-root successor");
      abort();
    }
    if (n != ~(uint32_t)0)
      n++;
    tail = hare;
    next = lj_obj_gcw_acq(hare);
    if (!next)
      goto done;
    if (power == lam) {
      tortoise = hare;
      power = power > ~(uint32_t)0 / 2u ? ~(uint32_t)0 : power << 1;
      lam = 0;
    }
    hare = next;
    if (lam != ~(uint32_t)0)
      lam++;
  }
  (void)gc_root_chain_break_cycle_held(g, head);
  goto restart;
done:
  *tailp = tail;
  return n;
}

static int gc_root_chain_contains_to_tail(global_State *g, GCobj *head, GCobj *tail,
					  GCobj *needle)
{
  GCobj *o;
  if (!needle)
    return 0;
  for (o = head; o != NULL; o = lj_obj_gcw_acq(o)) {
    if (LJ_UNLIKELY(!gc_root_link_valid(g, o))) {
      /* Never overwrite tail->next using an unvalidated existing-spine head.
      ** The detached pending chain remains intact up to this fail-stop. */
      lj_assertG(0, "invalid root spine during pending-root splice");
      abort();
    }
    if (o == needle)
      return 1;
    if (o == tail)
      return 0;
  }
  return 0;
}

static void gc_root_prepend_chain_at(global_State *g, GCRef *p, GCobj *head,
				     GCobj *tail)
{
  GCobj *oldhead;
  do {
    oldhead = gcref_acq(*p);
    /*
    ** Pending-root publication owns head..tail, but address reuse can make a
    ** freshly published pending chain overlap the existing GC root spine at
    ** the insertion point. Linking tail back to that old head would create a
    ** cycle; null-terminating instead preserves every unique object already
    ** reachable from head.
    */
    gc_root_set_next_rel(tail,
			 gc_root_chain_contains_to_tail(g, head, tail, oldhead) ?
			 NULL : oldhead);
  } while (!gcref_cas(p, &oldhead, head));
  lj_gcroot_repair_epoch_add(g);
}

static void gc_root_prepend_known_chain(global_State *g, GCobj *head,
					GCobj *tail)
{
  gc_root_prepend_chain_at(g, lj_gc_root_ref(g), head, tail);
}

static uint32_t gc_root_prepend_chain(global_State *g, GCobj *head)
{
  GCobj *tail;
  uint32_t n = gc_root_chain_tail(g, head, &tail);
  if (!n)
    return 0;
  gc_root_prepend_known_chain(g, head, tail);
  return n;
}

static uint32_t gc_root_prepend_chain_after(global_State *g, GCobj *anchor,
					    GCobj *head)
{
  GCobj *tail;
  uint32_t n = gc_root_chain_tail(g, head, &tail);
  if (!n)
    return 0;
  if (LJ_UNLIKELY(!anchor)) {
    lj_assertG(0, "missing main-thread pending-root anchor");
    abort();
  }
  gc_root_prepend_chain_at(g, lj_obj_gcwref(anchor), head, tail);
  /* mainthread is a permanent root-spine anchor. Root pruning never detaches
  ** it, so publishing after it needs exactly the one slot CAS above. Do not
  ** defensively reanchor by walking mainthread's live successor chain: the
  ** returned tail has no lifetime/membership lease and a concurrent prune can
  ** retire or reuse it before gc_root_prepend_chain_at() writes through it.
  ** If an already-corrupt spine has lost the permanent anchor, leaving this
  ** separately side-rooted chain detached is fail-closed; arena discovery
  ** retains it rather than performing a stale-tail write. */
  return n;
}

static uint32_t gc_flush_root_pending_tg(global_State *g, TGState *tg)
{
  GCobj *head;
  uint32_t n;
  if (!g || !tg || tg->gl != g)
    return 0;
  head = lj_tg_gcroot_pending_xchg_acqrel(tg, NULL);
  n = gc_root_prepend_chain(g, head);
  head = lj_tg_gcroot_pending_after_main_xchg_acqrel(tg, NULL);
  n += gc_root_prepend_chain_after(g, obj2gco(mainthread_acq(g)), head);
  return n;
}

static void gc_pending_root_stats(global_State *g, uint32_t n)
{
  uint64_t old;
  if (n == 0)
    return;
  gc2_pending_root_flushes_add(g, 1);
  gc2_pending_root_flushed_add(g, n);
  old = gc2_pending_root_flush_max_acq(g);
  while (old < (uint64_t)n) {
    uint64_t expect = old;
    if (gc2_pending_root_flush_max_cas(g, &expect, (uint64_t)n))
      break;
    old = expect;
  }
}

static int gc_root_pending_tg_nonempty(TGState *tg)
{
  return tg &&
    (lj_tg_gcroot_pending_acq(tg) != NULL ||
     lj_tg_gcroot_pending_after_main_acq(tg) != NULL);
}

uint32_t lj_gc_flush_root_pending(global_State *g)
{
  TGState *tg, *main_tg, *self;
  uint32_t n = 0, saw_main = 0, saw_self = 0;
  int reclaim_held;
  if (!g)
    return 0;
  main_tg = g->main_tg;
  self = lj_thr_get_tg();
  /* Raw TLS may legitimately belong to another independent Lua universe on
  ** the same OS thread. It is neither a pending-root producer nor an attach
  ** edge for this registry; exclude it before the hint fast path inspects its
  ** owner-private chains. gc_flush_root_pending_tg() also rejects mismatched
  ** universes, but that is deliberately after the non-empty probe. */
  if (self && self->gl != g)
    self = NULL;
  /*
  ** This is only a non-empty hint. Publishers set it before and after pending
  ** stack publication; false positives are harmless and false negatives are
  ** covered for the two TGs that can publish without already being visible in
  ** gc2.tg_list: the main TG and the TLS-current TG during attach/detach edges.
  */
  if (lj_gcroot_pending_hint_acq(g) == 0 &&
      !gc_root_pending_tg_nonempty(main_tg) &&
      (self == NULL || self == main_tg ||
       !gc_root_pending_tg_nonempty(self)))
    return 0;
  /* Pin the TG/HugeTab registry before the first pending-head xchg and retain
  ** it through validation and splice. The exclusive reclaimer already owns a
  ** stronger exact-thread registry certificate; every other caller uses one
  ** nonblocking ordinary reader. On admission loss leave the hint and every
  ** per-TG chain untouched for a later flush. */
  reclaim_held = lj_gc2_reclaim_context_held(g);
  if (!reclaim_held && !lj_gc2_smr_read_try(g))
    return 0;
  /*
  ** Once the hint is non-zero, clear it before scanning so a concurrent
  ** publisher can republish a new non-empty state. The zero-hint fast return
  ** above intentionally avoids a global RMW on the overwhelmingly common empty
  ** flush path; concurrent remote publishers that set the hint after the load
  ** are covered by the next flush, matching the old xchg-after-zero race.
  */
  (void)lj_gcroot_pending_hint_xchg(g, 0);
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg)) {
    if (tg == main_tg)
      saw_main = 1;
    if (tg == self)
      saw_self = 1;
    n += gc_flush_root_pending_tg(g, tg);
  }
  if (main_tg && !saw_main)
    n += gc_flush_root_pending_tg(g, main_tg);
  if (self && self != main_tg && !saw_self)
    n += gc_flush_root_pending_tg(g, self);
  if (n != 0)
    (void)lj_gc_repair_root_spine(g);
  gc_pending_root_stats(g, n);
  if (!reclaim_held)
    lj_gc2_smr_read_leave(g);
  return n;
}

static int gc_linkobj_pending(global_State *g, GCobj *o)
{
  TGState *tg = lj_thr_get_tg();
  GCRootStateRef rootstate;
  GCobj *head;
  int claim;
  if (!g || !o)
    return LJ_GC_ROOT_LINK_INVALID;
  /* Closed open-upvalues have exact-base identity and remain incarnation-
  ** pinned by their closure/open-list handoff throughout this requeue. */
  claim = gc_root_link_claim_at(g, o, o, &rootstate);
  if (claim != LJ_GC_ROOT_LINKED)
    return claim;
  if (!tg || tg->gl != g || lj_tg_flags_test_acq(tg, TGF_DEAD)) {
    return gc_root_publish_claimed(g, o, &rootstate);
  }
  /* A worker/attacher can publish its activation immediately after any
  ** single-threaded eligibility sample and exchange this stack before the
  ** following store. Always couple the acquired head to publication with the
  ** CAS: on a racing flush, failure rebuilds o->gcw from the fresh head instead
  ** of publishing a detached tail whose address may later be reused. */
  head = lj_tg_gcroot_pending_acq(tg);
  gc_test_root_pending_loaded(g, tg, o, head,
			      LJ_GC_ROOT_PENDING_TEST_ORDINARY);
  do {
    gc_root_set_next_rel(o, head);
  } while (!lj_tg_gcroot_pending_cas(tg, &head, o));
  if (LJ_UNLIKELY(!gc_root_link_commit(&rootstate))) {
    lj_assertG(0, "pending root membership commit lost");
    return LJ_GC_ROOT_LINK_DEFER;
  }
  return LJ_GC_ROOT_LINKED;
}

int lj_gc_linkobj_pending(global_State *g, GCobj *o)
{
  return gc_linkobj_pending(g, o);
}

static int gc_linkobj_new_pending_at(global_State *g, GCobj *o, void *base)
{
  TGState *tg = lj_thr_get_tg();
  GCRootStateRef rootstate;
  GCobj *head;
  int claim;
  if (!g || !o || !base)
    return LJ_GC_ROOT_LINK_INVALID;
  if (LJ_UNLIKELY(!gc_publishobj_header_at(g, o, base))) {
    lj_assertG(0, "fresh GC object lost exact header publication");
    abort();
  }
  claim = gc_root_construct_claimed_at(g, o, base, &rootstate);
  if (LJ_UNLIKELY(claim != LJ_GC_ROOT_LINKED)) {
    lj_assertG(0, "fresh GC object lost construction ownership");
    abort();
  }
  if (!tg || tg->gl != g || lj_tg_flags_test_acq(tg, TGF_DEAD))
    return gc_root_publish_constructed(g, o, &rootstate);
  head = lj_tg_gcroot_pending_acq(tg);
  gc_test_root_pending_loaded(g, tg, o, head,
			      LJ_GC_ROOT_PENDING_TEST_ORDINARY);
  do {
    gc_root_set_next_rel(o, head);
  } while (!lj_tg_gcroot_pending_cas(tg, &head, o));
  if (LJ_UNLIKELY(!gc_root_construct_commit(&rootstate))) {
    lj_assertG(0, "pending constructed root membership commit lost");
    abort();
  }
  return LJ_GC_ROOT_LINKED;
}

int lj_gc_linkobj_new(global_State *g, GCobj *o)
{
  return gc_linkobj_new_pending_at(g, o, o);
}

int lj_gc_linkobj_new_at(global_State *g, GCobj *o, void *base)
{
  if (!base)
    return LJ_GC_ROOT_LINK_INVALID;
  return gc_linkobj_new_pending_at(g, o, base);
}

static int gc_root_chain_commit_constructed(global_State *g, GCobj *head,
					    GCobj *tail)
{
  GCobj *o = head;
  for (;;) {
    GCRootStateRef rootstate;
    GCobj *next;
    if (gc_root_construct_claimed_at(g, o, o, &rootstate) !=
	LJ_GC_ROOT_LINKED ||
	!gc_root_construct_commit(&rootstate))
      return 0;
    if (o == tail)
      return 1;
    next = lj_obj_gcw_acq(o);
    if (!next || next == o)
      return 0;
    o = next;
  }
}

int lj_gc_linkobj_new_chain(global_State *g, GCobj *head, GCobj *tail)
{
  TGState *tg;
  GCobj *oldhead;
  if (!head || !tail)
    return LJ_GC_ROOT_LINK_INVALID;
  {
    GCobj *o = head;
    for (;;) {
      GCRootStateRef rootstate;
      GCobj *next;
      int claim;
      if (LJ_UNLIKELY(!gc_publishobj_header_at(g, o, o))) {
	lj_assertG(0, "fresh GC chain lost exact header publication");
	abort();
      }
      claim = gc_root_construct_claimed_at(g, o, o, &rootstate);
      if (claim != LJ_GC_ROOT_LINKED) {
	lj_assertG(0, "fresh GC chain lost construction ownership");
	abort();
      }
      if (o == tail)
	break;
      next = lj_obj_gcw_acq(o);
      lj_assertG(next != NULL && next != o,
		 "invalid unpublished GC object chain");
      if (!next || next == o) {
	abort();
      }
      o = next;
    }
  }
  tg = lj_thr_get_tg();
  if (!tg || tg->gl != g || lj_tg_flags_test_acq(tg, TGF_DEAD)) {
    gc_root_prepend_known_chain(g, head, tail);
  } else {
    /* See gc_linkobj_pending(): the tail link and pending-stack head must
    ** linearize together even before MT becomes sticky. */
    oldhead = lj_tg_gcroot_pending_acq(tg);
    gc_test_root_pending_loaded(g, tg, tail, oldhead,
				LJ_GC_ROOT_PENDING_TEST_CHAIN);
    do {
      gc_root_set_next_rel(tail, oldhead);
    } while (!lj_tg_gcroot_pending_cas(tg, &oldhead, head));
  }
  if (LJ_UNLIKELY(!gc_root_chain_commit_constructed(g, head, tail))) {
    lj_assertG(0, "constructed root chain membership commit lost");
    abort();
  }
  return LJ_GC_ROOT_LINKED;
}

int lj_gc_linkobj_new_chain_arena(global_State *g, GCArena *a,
					   GCobj *head, uint32_t headcell,
					   GCobj *tail, uint32_t tailcell)
{
  TGState *tg = lj_thr_get_tg();
  GCobj *oldhead;
  if (!g || !a || !head || !tail || head == tail ||
      !tg || tg != g->main_tg || tg->gl != g ||
      lj_tg_flags_test_acq(tg, TGF_DEAD) ||
      mt_active_acq(g) != 0 || mt_entering_acq(g) != 0 ||
      gc2_n_workers_acq(g) != 0 || lj_arena_ishuge(a) ||
      (lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE) == 0 ||
      lj_arena_owner_acq(a) != lj_tg_tid_acq(tg) ||
      headcell < LJ_AFIRST_CELL || headcell >= LJ_ARENA_CELLS ||
      tailcell < LJ_AFIRST_CELL || tailcell >= LJ_ARENA_CELLS ||
      lj_arena_cellptr(a, headcell) != (void *)head ||
      lj_arena_cellptr(a, tailcell) != (void *)tail ||
      !lj_arena_bm_get(a->block, headcell) ||
      !lj_arena_bm_get(a->block, tailcell) ||
      !lj_arena_ready_get(a, headcell) ||
      !lj_arena_ready_get(a, tailcell) ||
      lj_arena_lifetime_state_acq(a, headcell) !=
	LJ_ARENA_LIFETIME_CONSTRUCT ||
      lj_arena_lifetime_state_acq(a, tailcell) !=
	LJ_ARENA_LIFETIME_CONSTRUCT ||
      lj_arena_root_state_acq(a, headcell) != LJ_ARENA_ROOT_LINKING ||
      lj_arena_root_state_acq(a, tailcell) != LJ_ARENA_ROOT_LINKING ||
      lj_obj_gcw_acq(head) != tail)
    return lj_gc_linkobj_new_chain(g, head, tail);

  /* Both exact constructor lanes and READY headers were installed by this
  ** sole VM/allocator thread, with no GC-capable operation in between. Publish
  ** the pair in one pending-stack CAS and commit those already-known lanes;
  ** avoid rediscovering each fresh address through the shared registry. */
  oldhead = lj_tg_gcroot_pending_acq(tg);
  gc_test_root_pending_loaded(g, tg, tail, oldhead,
			      LJ_GC_ROOT_PENDING_TEST_CHAIN);
  do {
    gc_root_set_next_rel(tail, oldhead);
  } while (!lj_tg_gcroot_pending_cas(tg, &oldhead, head));
  if (LJ_UNLIKELY(!lj_arena_root_construct_commit_pair(
		    a, headcell, tailcell))) {
    lj_assertG(0, "fast constructed arena chain commit lost");
    abort();
  }
  return LJ_GC_ROOT_LINKED;
}

int lj_gc_linkobj_new_after_main(global_State *g, GCobj *o)
{
  TGState *tg = lj_thr_get_tg();
  GCRootStateRef rootstate;
  GCobj *head;
  int claim;
  if (!g || !o)
    return LJ_GC_ROOT_LINK_INVALID;
  if (LJ_UNLIKELY(!gc_publishobj_header_at(g, o, o))) {
    lj_assertG(0, "fresh after-main object lost exact header publication");
    abort();
  }
  claim = gc_root_construct_claimed_at(g, o, o, &rootstate);
  if (LJ_UNLIKELY(claim != LJ_GC_ROOT_LINKED)) {
    lj_assertG(0, "fresh after-main object lost construction ownership");
    abort();
  }
  if (!tg || tg->gl != g || lj_tg_flags_test_acq(tg, TGF_DEAD)) {
    GCobj *anchor = obj2gco(mainthread_acq(g));
    GCRef *p;
    if (!anchor)
      abort();
    p = lj_obj_gcwref(anchor);
    do {
      head = gcref_acq(*p);
      gc_root_set_next(o, head);
    } while (!gcref_cas(p, &head, o));
    if (LJ_UNLIKELY(!gc_root_construct_commit(&rootstate))) {
      lj_assertG(0, "after-main constructed membership commit lost");
      abort();
    }
    lj_gcroot_repair_epoch_add(g);
    return LJ_GC_ROOT_LINKED;
  }
  /* The after-main stack shares the same activation race as the ordinary
  ** pending stack. A failed CAS rewrites o->gcw before any reader can discover
  ** o through this publication. */
  head = lj_tg_gcroot_pending_after_main_acq(tg);
  gc_test_root_pending_loaded(g, tg, o, head,
			      LJ_GC_ROOT_PENDING_TEST_AFTER_MAIN);
  do {
    gc_root_set_next_rel(o, head);
  } while (!lj_tg_gcroot_pending_after_main_cas(tg, &head, o));
  if (LJ_UNLIKELY(!gc_root_construct_commit(&rootstate))) {
    lj_assertG(0, "pending after-main constructed membership commit lost");
    abort();
  }
  return LJ_GC_ROOT_LINKED;
}

int lj_gc_linkobj_after(global_State *g, GCobj *anchor, GCobj *o)
{
  GCRootStateRef rootstate;
  GCRef *p;
  GCobj *head;
  int claim;
  if (!anchor || !o)
    return LJ_GC_ROOT_LINK_INVALID;
  /* Userdata FINREG keeps the fixed-layout node and registration flag stable
  ** until this anchored requeue has committed. */
  claim = gc_root_link_claim_at(g, o, o, &rootstate);
  if (claim != LJ_GC_ROOT_LINKED)
    return claim;
  p = lj_obj_gcwref(anchor);
  do {
    head = gcref_acq(*p);
    gc_root_set_next(o, head);
  } while (!gcref_cas(p, &head, o));
  if (LJ_UNLIKELY(!gc_root_link_commit(&rootstate))) {
    lj_assertG(0, "anchored root membership commit lost");
    return LJ_GC_ROOT_LINK_DEFER;
  }
  lj_gcroot_repair_epoch_add(g);
  return LJ_GC_ROOT_LINKED;
}

int lj_gc_linkobj_terminal(global_State *g, GCobj *o)
{
  GCRootStateRef rootstate;
  uint32_t state;
  int kind;
  if (!g || !o)
    return LJ_GC_ROOT_LINK_INVALID;
  lj_gc_publishobj_header(g, o);
  kind = gc_root_state_ref(g, o, &rootstate);
  if (kind == GC_ROOT_STATE_INVALID)
    return LJ_GC_ROOT_LINK_INVALID;
  if (kind != GC_ROOT_STATE_EXEMPT) {
    state = gc_root_state_acq(&rootstate);
    if (state == LJ_ARENA_ROOT_MEMBER) {
	if (!gc_root_state_cas(&rootstate, state, LJ_ARENA_ROOT_UNLINKING) ||
	    !gc_root_unlink_commit(g, &rootstate))
	return LJ_GC_ROOT_LINK_DEFER;
    } else if (state == LJ_ARENA_ROOT_LINKING) {
      (void)gc_root_link_terminal_rollback(g, &rootstate);
      if (gc_root_state_acq(&rootstate) != LJ_ARENA_ROOT_NONE)
	return LJ_GC_ROOT_LINK_DEFER;
    } else if (state == LJ_ARENA_ROOT_UNLINKING) {
      if (!gc_root_unlink_commit(g, &rootstate))
	return LJ_GC_ROOT_LINK_DEFER;
    } else if (state != LJ_ARENA_ROOT_NONE) {
      return LJ_GC_ROOT_LINK_DEFER;
    }
  }
  return lj_gc_linkobj_at(g, o, rootstate.base);
}

/* Allocate a pending GC object. Header/body initialization must finish before
** lj_gc_linkobj_new() publishes READY and the ownership root. Legacy non-x64
** JIT backends must use the same explicit final publication contract before
** they are brought into the current target set. */
void * LJ_FASTCALL lj_mem_newgco(lua_State *L, GCSize size)
{
  global_State *g = G(L);
  GCobj *o = (GCobj *)lj_mem_newgco_raw(
    L, size, LJ_AF_TRAVERSABLE|LJ_AF_ROOT_CONSTRUCT);
  newwhite(g, o);
  return o;
}

int lj_mem_abandon_gco_unpublished(global_State *g, void *base)
{
  GCArena *a;
  LJArenaAllocD *ad;
  if (!g || !base)
    return LJ_ARENA_HUGE_ROOT_COMPLETE_LOST;
  if (g->allocf != lj_arena_allocf ||
      la_load32_acq(&g->allocf_arena) == 0)
    return LJ_ARENA_HUGE_ROOT_COMPLETE_LIVE;
  if (!checkptrGC(base))
    return LJ_ARENA_HUGE_ROOT_COMPLETE_LOST;
  /* The caller owns this exact allocation's constructor lane. Resolving that
  ** lane through the global arena registry would add no identity proof and can
  ** fail spuriously while an unrelated opportunistic SMR reclaimer owns its
  ** writer gate. Validate the immutable arena owner locally instead; this is
  ** the same constructor-only route used by fresh root publication. */
  a = lj_arena_of(base);
  ad = gc_constructor_allocd_at(g, base);
  if (!ad)
    return LJ_ARENA_HUGE_ROOT_COMPLETE_LOST;
  if (!lj_arena_ishuge(a)) {
    uint32_t cell = lj_arena_cellof(base);
    if (cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS ||
	lj_arena_cellptr(a, cell) != base ||
	!(lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE) ||
	!lj_arena_bm_get(a->block, cell) ||
	!lj_arena_root_construct_abandon(a, cell))
      return LJ_ARENA_HUGE_ROOT_COMPLETE_LOST;
    return LJ_ARENA_HUGE_ROOT_COMPLETE_LIVE;
  } else {
    int result;
    if (!ad->huge)
      return LJ_ARENA_HUGE_ROOT_COMPLETE_LOST;
    result = lj_arena_hugetab_root_construct_abandon(
	ad->huge, base, lj_gc2_retire_epoch(g), NULL);
    if (result == LJ_ARENA_HUGE_ROOT_COMPLETE_SWEEP) {
      gc2_sweep_grace_needed_rel(g, 1);
      lj_gc2_sweep_publish_wake(g);
    }
    return result;
  }
}

void lj_mem_freegco_unpublished(global_State *g, void *base, GCSize osize)
{
  int result = lj_mem_abandon_gco_unpublished(g, base);
  if (LJ_UNLIKELY(result != LJ_ARENA_HUGE_ROOT_COMPLETE_LIVE)) {
    /* SWEEP means a racing logical free already owns both accounting and
    ** physical reclaim. LOST cannot be made safe by guessing at the bytes. */
    if (result == LJ_ARENA_HUGE_ROOT_COMPLETE_SWEEP)
      return;
    lj_assertG(0, "unpublished GC construction ownership lost");
    abort();
  }
  lj_mem_free(g, base, osize);
}

void lj_mem_free(global_State *g, void *p, size_t osize)
{
  lj_gc_total_sub(g, (GCSize)osize);
  if (g->allocf == lj_arena_allocf) {
    LJArenaAllocD *ad = gc_arena_allocd_for_ptr(g, p);
    (void)lj_arena_allocf(ad, p, osize, 0);
  } else {
    g->allocf(g->allocd, p, osize, 0);
  }
}

/* Resize growable vector. */
void *lj_mem_grow(lua_State *L, void *p, MSize *szp, MSize lim, MSize esz)
{
  MSize sz = (*szp) << 1;
  if (sz < LJ_MIN_VECSZ)
    sz = LJ_MIN_VECSZ;
  if (sz > lim)
    sz = lim;
  p = lj_mem_realloc(L, p, (*szp)*esz, sz*esz);
  *szp = sz;
  return p;
}

/*
** Account a dead traversable GC object body for later arena bitmap reclaim.
** A successful deferral means the type-specific destructor has finished and
** the body is released from the live bitmap without being inserted into an
** allocator free list for immediate reuse. Keep the object header intact until
** a later free-list rebuild owns reuse: VM/JIT readers can still hold
** SMR-protected stale pointers during the deferral window.
*/
int lj_mem_freegco_defer(global_State *g, void *p, GCSize osize)
{
  TGState *tg = G2TG(g);
  GCArena *a;
  uint32_t cell;
  if (!p || !tg || !lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL) ||
      g->allocf != lj_arena_allocf)
    return 0;
  a = lj_arena_of(p);
  if (lj_arena_ishuge(a) || !(a->hdr.flags & LJ_AF_TRAVERSABLE))
    return 0;
  cell = lj_arena_cellof(p);
  if (cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS ||
      !lj_arena_bm_get(a->block, cell))
    return 0;
  if (!lj_arena_free_deferred(&tg->alloc, p, osize))
    return 0;
  lj_gc_total_sub(g, osize);
  return 1;
}
