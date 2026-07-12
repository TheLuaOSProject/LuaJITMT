/*
** Trace management.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_trace_c
#define LUA_CORE

#include "lj_obj.h"

#if LJ_HASJIT

#include "lj_atomic.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_err.h"
#include "lj_debug.h"
#include "lj_str.h"
#include "lj_frame.h"
#include "lj_state.h"
#include "lj_bc.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_iropt.h"
#include "lj_mcode.h"
#include "lj_trace.h"
#include "lj_snap.h"
#include "lj_gdbjit.h"
#include "lj_record.h"
#include "lj_asm.h"
#include "lj_safepoint.h"
#include "lj_dispatch.h"
#include "lj_thr.h"
#include "lj_tg.h"
#if LJ_HASPROFILE
#include "lj_profile.h"
#endif
#include "lj_vm.h"
#include "lj_vmevent.h"
#include "lj_target.h"
#include "lj_prng.h"

#ifdef LJ_TRACE_TEST_HELPERS
static uint32_t trace_test_call_unroll_aborts;
static uint32_t trace_test_call_unroll_linked;
static uint32_t trace_test_flush_unlink_calls;
static uint32_t trace_test_flush_unlink_returns;
static uint32_t trace_test_abort_selflinks;
static uint32_t trace_test_slot_release_calls;
static uint32_t trace_test_slot_release_clears;
static uint32_t trace_test_findfree_calls;
static uint32_t trace_test_findfree_reuses;
static uint32_t trace_test_findfree_grows;
static uint32_t trace_test_last_unlinked;
static uint32_t trace_test_last_findfree;
static uint32_t trace_test_last_released;

#define TRACE_TEST_COUNTER(name) \
uint32_t lj_trace_test_##name(void) \
{ \
  return la_load32_acq(&trace_test_##name); \
}

void lj_trace_test_reset_retention_stats(void)
{
  la_store32_rel(&trace_test_call_unroll_aborts, 0);
  la_store32_rel(&trace_test_call_unroll_linked, 0);
  la_store32_rel(&trace_test_flush_unlink_calls, 0);
  la_store32_rel(&trace_test_flush_unlink_returns, 0);
  la_store32_rel(&trace_test_abort_selflinks, 0);
  la_store32_rel(&trace_test_slot_release_calls, 0);
  la_store32_rel(&trace_test_slot_release_clears, 0);
  la_store32_rel(&trace_test_findfree_calls, 0);
  la_store32_rel(&trace_test_findfree_reuses, 0);
  la_store32_rel(&trace_test_findfree_grows, 0);
  la_store32_rel(&trace_test_last_unlinked, 0);
  la_store32_rel(&trace_test_last_findfree, 0);
  la_store32_rel(&trace_test_last_released, 0);
}

void lj_trace_test_note_call_unroll_abort(TraceNo lnk)
{
  (void)la_add32_acqrel(&trace_test_call_unroll_aborts, 1);
  if (lnk)
    (void)la_add32_acqrel(&trace_test_call_unroll_linked, 1);
}

TRACE_TEST_COUNTER(call_unroll_aborts)
TRACE_TEST_COUNTER(call_unroll_linked)
TRACE_TEST_COUNTER(flush_unlink_calls)
TRACE_TEST_COUNTER(flush_unlink_returns)
TRACE_TEST_COUNTER(abort_selflinks)
TRACE_TEST_COUNTER(slot_release_calls)
TRACE_TEST_COUNTER(slot_release_clears)
TRACE_TEST_COUNTER(findfree_calls)
TRACE_TEST_COUNTER(findfree_reuses)
TRACE_TEST_COUNTER(findfree_grows)
TRACE_TEST_COUNTER(last_unlinked)
TRACE_TEST_COUNTER(last_findfree)
TRACE_TEST_COUNTER(last_released)

static LJ_AINLINE void trace_test_note_flush_unlink(GCtrace *T,
						    TraceNo traceno)
{
  (void)la_add32_acqrel(&trace_test_flush_unlink_calls, 1);
  if (trace_linktype_acq(T) == LJ_TRLINK_RETURN)
    (void)la_add32_acqrel(&trace_test_flush_unlink_returns, 1);
  la_store32_rel(&trace_test_last_unlinked, (uint32_t)traceno);
}

static LJ_AINLINE void trace_test_note_abort_selflink(TraceNo traceno)
{
  (void)la_add32_acqrel(&trace_test_abort_selflinks, 1);
  la_store32_rel(&trace_test_last_released, (uint32_t)traceno);
}

static LJ_AINLINE void trace_test_note_slot_release(TraceNo traceno,
						   int cleared)
{
  (void)la_add32_acqrel(&trace_test_slot_release_calls, 1);
  if (cleared)
    (void)la_add32_acqrel(&trace_test_slot_release_clears, 1);
  la_store32_rel(&trace_test_last_released, (uint32_t)traceno);
}

static LJ_AINLINE void trace_test_note_findfree_reuse(TraceNo traceno)
{
  (void)la_add32_acqrel(&trace_test_findfree_reuses, 1);
  la_store32_rel(&trace_test_last_findfree, (uint32_t)traceno);
}

static LJ_AINLINE void trace_test_note_findfree_grow(TraceNo traceno)
{
  (void)la_add32_acqrel(&trace_test_findfree_grows, 1);
  la_store32_rel(&trace_test_last_findfree, (uint32_t)traceno);
}
#else
#define trace_test_note_flush_unlink(T, traceno) \
  ((void)(T), (void)(traceno))
#define trace_test_note_abort_selflink(traceno)		((void)(traceno))
#define trace_test_note_slot_release(traceno, cleared) \
  ((void)(traceno), (void)(cleared))
#define trace_test_note_findfree_reuse(traceno)		((void)(traceno))
#define trace_test_note_findfree_grow(traceno)		((void)(traceno))
#endif

static int trace_scope_mark_pending(GCtrace *T)
{
  uint8_t flags = la_load8_acq(&T->unused1);
  while ((flags & TRACE_SCOPE_FLUSH_PENDING) == 0) {
    uint8_t next = (uint8_t)(flags | TRACE_SCOPE_FLUSH_PENDING);
    if (la_cas8(&T->unused1, &flags, next, LA_ACQ_REL, LA_ACQ))
      return 1;
  }
  return 0;
}

/* Defined with the trace-graph mutation helpers below. Retirement publishes
** the body first, then disconnects every semantic entry edge while the same
** recorder-token owner still excludes assembler/link publication. */
static void trace_retire_disconnect(jit_State *J, GCtrace *T);

/* -- Error handling ------------------------------------------------------ */

TGState *lj_jit_owner_tg_l(lua_State *L, jit_State *J)
{
  global_State *g = J2G(J);
  TGState *tg;
  uint32_t owner;
  if (!L || G(L) != g)
    return NULL;
  /*
  ** Recorder ownership is tied to the lua_State that started recording. Do not
  ** fall back through ambient TLS here: a different thread can observe J->L and
  ** otherwise mistake its own TG for the active recorder owner.
  */
  tg = L->tg_hint;
  if (tg && tg->gl == g)
    return tg;
  owner = lj_state_owner_acq(L);
  return owner != 0 && owner != LJ_THREAD_GCSCAN ?
	 lj_tg_find_owner(g, owner) : NULL;
}

static LJ_AINLINE uint32_t jit_token_tid_l(lua_State *L, jit_State *J)
{
  TGState *tg = lj_jit_owner_tg_l(L, J);
  return tg ? lj_tg_tid_acq(tg) : 0;
}

int lj_jit_token_try(jit_State *J)
{
  global_State *g = J2G(J);
  TGState *tg = J2TG(J);
  uint32_t tid = tg ? lj_tg_tid_acq(tg) : 0;
  uint32_t expect = 0;
  if (tid == 0)
    return 0;
  return jit_token_cas(g, &expect, tid);
}

int lj_jit_token_try_l(lua_State *L, jit_State *J)
{
  global_State *g = J2G(J);
  uint32_t tid = jit_token_tid_l(L, J);
  uint32_t expect = 0;
  return tid != 0 && jit_token_cas(g, &expect, tid);
}

int lj_jit_token_held(jit_State *J)
{
  global_State *g = J2G(J);
  TGState *tg = J2TG(J);
  uint32_t tid = tg ? lj_tg_tid_acq(tg) : 0;
  return tid != 0 && jit_token_acq(g) == tid;
}

int lj_jit_token_held_l(lua_State *L, jit_State *J)
{
  uint32_t tid = jit_token_tid_l(L, J);
  return tid != 0 && jit_token_acq(J2G(J)) == tid;
}

void lj_jit_token_release(jit_State *J)
{
  global_State *g = J2G(J);
  TGState *tg = J2TG(J);
  uint32_t tid = tg ? lj_tg_tid_acq(tg) : 0;
  if (tid != 0 && jit_token_acq(g) == tid) {
    if (lj_trace_state_load(J) == LJ_TRACE_IDLE)
      jit_owner_l_rel(J, NULL);  /* Never publish an idle detachable state. */
    jit_token_rel(g, 0);
  }
}

void lj_jit_token_release_l(lua_State *L, jit_State *J)
{
  global_State *g = J2G(J);
  uint32_t tid = jit_token_tid_l(L, J);
  if (tid != 0 && jit_token_acq(g) == tid) {
    if (lj_trace_state_load(J) == LJ_TRACE_IDLE)
      jit_owner_l_rel(J, NULL);  /* The explicit L is valid for this release. */
    jit_token_rel(g, 0);
  }
}

int lj_jit_token_acquire_wait(jit_State *J)
{
  TGState *tg = J2TG(J);
  lua_State *L = tg ? lj_tg_load_cur_L(tg) : NULL;
  int had_stopreq = lj_safepoint_had_stopreq(L);
  if (lj_jit_token_held(J))
    return 0;
  for (;;) {
    if (lj_jit_token_try(J))
      return 1;
    lj_trace_state_abort(J);
    lj_safepoint_checkstop_fresh(L, lj_thr_retry_yield(L), had_stopreq);
  }
}

void lj_trace_abort(global_State *g)
{
  jit_State *J = G2J(g);
  lj_trace_state_abort(J);
}

/* Synchronous abort with error message. */
void lj_trace_err(jit_State *J, TraceError e)
{
  setnilV(&J->errinfo);  /* No error info. */
  setintV(J->L->top++, (int32_t)e);
  lj_err_throw(J->L, LUA_ERRRUN);
}

/* Synchronous abort with error message and error info. */
void lj_trace_err_info(jit_State *J, TraceError e)
{
  setintV(J->L->top++, (int32_t)e);
  lj_err_throw(J->L, LUA_ERRRUN);
}

/* -- Trace management ---------------------------------------------------- */

/* The current trace is first assembled in J->cur. The variable length
** arrays point to shared, growable buffers (J->irbuf etc.). When trace
** recording ends successfully, the current trace and its data structures
** are copied to a new (compact) GCtrace object.
*/

static TraceVec *tracevec_new(lua_State *L, MSize sizetrace)
{
  TraceVec *tv = (TraceVec *)lj_mem_new(L, tracevec_size(sizetrace));
  tv->sizetrace = sizetrace;
  tv->retire_epoch = 0;
  tracevec_retired_next_rel(tv, NULL);
  memset(tv->slot, 0, sizetrace*sizeof(GCRef));
  return tv;
}

static void tracevec_free(global_State *g, TraceVec *tv)
{
  lj_mem_free(g, tv, tracevec_size(tv->sizetrace));
}

static void tracevec_publish(jit_State *J, TraceVec *tv)
{
  trace_sizetrace_rel(J, tv->sizetrace);
  tracevec_rel(J, tv);
}

static void tracevec_retired_push(jit_State *J, TraceVec *tv)
{
  TraceVec *head = tracevec_retired_head_acq(J);
  do {
    tracevec_retired_next_rel(tv, head);
  } while (!tracevec_retired_head_cas(J, &head, tv));
  /* 08 section 8.3 RCU retire. */
}

static void tracevec_preserve_retired(global_State *g, TraceVec *tv)
{
  if (tv && lj_gc2_mem_registered(g, tv))
    (void)lj_gc2_markmem_registered(g, tv);
}

static void tracevec_retire(jit_State *J, TraceVec *tv)
{
  if (tv) {
    global_State *g = J2G(J);
    la_store64_rel(&tv->retire_epoch, lj_gc2_retire_epoch(J2G(J)));
    /*
    ** Retired trace vectors are raw arena records, not ordinary Lua roots.
    ** Preserve both sides of publication: the first mark protects against an
    ** in-progress sweep, the second covers a GC2 root scan that starts between
    ** the mark and the CAS push. A later idle cycle clears stale marks before
    ** it starts tracing.
    */
    tracevec_preserve_retired(g, tv);
    tracevec_retired_push(J, tv);
    tracevec_preserve_retired(g, tv);
  }
}

static void trace_exittab_free(global_State *g, GCtrace *T, SnapNo nsnap);
typedef struct TraceProtoPCState TraceProtoPCState;
static void trace_preservebody(global_State *g, GCtrace *T,
			       TraceProtoPCState *pcstate);
static void trace_preservebody_reclaim(global_State *g, GCtrace *T,
				       TraceProtoPCState *pcstate);

static LJArenaAllocD *trace_arena_allocd_for_tg(global_State *g, TGState *tg)
{
  if (tg && lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL))
    return &tg->allocd;
  return (LJArenaAllocD *)g->allocd;
}

static LJArenaAllocD *trace_arena_allocd_for_ptr(global_State *g, const void *p)
{
  if (p) {
    uint32_t owner_tid = lj_arena_owner_acq(lj_arena_of(p));
    TGState *tg = lj_tg_find_owner(g, owner_tid);
    if (tg)
      return trace_arena_allocd_for_tg(g, tg);
  }
  return (LJArenaAllocD *)g->allocd;
}

static int trace_body_fits_alloc(global_State *g, GCtrace *T, GCSize size)
{
  GCArena *a;
  uint32_t cell, maxcells;
  if (g->allocf != lj_arena_allocf)
    return 1;
  if (!T || size > LJ_MAX_MEM32)
    return 0;
  a = lj_arena_of(T);
  if (lj_arena_ishuge(a)) {
    LJArenaAllocD *ad = trace_arena_allocd_for_ptr(g, T);
    LJHugeInfo hi;
    return ad && ad->huge &&
	   lj_arena_hugetab_lookup(ad->huge, T, &hi) == 1 &&
	   hi.size == size;
  }
  cell = lj_arena_cellof(T);
  if (cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS)
    return 0;
  maxcells = LJ_ARENA_CELLS - cell;
  return lj_arena_ncells(size) <= maxcells;
}

static int trace_size_checked(global_State *g, GCtrace *T, GCSize *sizep,
			      SnapNo *nsnapp)
{
  IRRef nins, nk;
  SnapNo nsnap;
  MSize nsnapmap;
  GCSize size = (GCSize)((sizeof(GCtrace)+7)&~7);
  GCSize nref;
  const MSize snapmap_per_snap =
    (MSize)LJ_MAX_JSLOTS + (MSize)LJ_STACK_EXTRA + 32u;
  if (!T || !checkptrGC(T) || T->gct != (uint32_t)~LJ_TTRACE)
    return 0;
  nins = trace_nins_acq(T);
  nk = trace_nk_acq(T);
  nsnap = trace_nsnap_acq(T);
  nsnapmap = trace_nsnapmap_acq(T);
  /*
  ** A trace body's allocation size is reconstructed from its geometry at free
  ** time. Retired traces are still visible to stale exits and bytecode readers
  ** until their SMR epoch expires, so close/reclaim can observe a stale body if a
  ** duplicate retire entry slipped through. Validate the format invariants before
  ** using the header as an allocator contract.
  */
  if (nins < REF_BASE || nins > 0xffffu || nk > REF_BIAS || nk > nins)
    return 0;
  if ((nsnap == 0 && nsnapmap != 0) ||
      (nsnap != 0 && nsnapmap > (MSize)nsnap * snapmap_per_snap))
    return 0;
  nref = (GCSize)(nins - nk);
  if (nref > ((GCSize)LJ_MAX_MEM32 - size) / (GCSize)sizeof(IRIns))
    return 0;
  size += nref * (GCSize)sizeof(IRIns);
  if ((GCSize)nsnap > ((GCSize)LJ_MAX_MEM32 - size) /
			(GCSize)sizeof(SnapShot))
    return 0;
  size += (GCSize)nsnap * (GCSize)sizeof(SnapShot);
  if ((GCSize)nsnapmap > ((GCSize)LJ_MAX_MEM32 - size) /
			   (GCSize)sizeof(SnapEntry))
    return 0;
  size += (GCSize)nsnapmap * (GCSize)sizeof(SnapEntry);
  if (!trace_body_fits_alloc(g, T, size))
    return 0;
  *sizep = size;
  *nsnapp = nsnap;
  return 1;
}

static int trace_body_refs_valid(global_State *g, GCtrace *T, SnapNo *nsnapp)
{
  GCSize size;
  SnapNo nsnap;
  IRRef nins, nk;
  MSize nsnapmap;
  char *p;
  IRIns *irbase;
  SnapShot *snap;
  SnapEntry *snapmap;
  if (LJ_UNLIKELY(!trace_size_checked(g, T, &size, &nsnap)))
    return 0;
  nins = trace_nins_acq(T);
  nk = trace_nk_acq(T);
  nsnapmap = trace_nsnapmap_acq(T);
  p = (char *)T + ((sizeof(GCtrace)+7)&~7);
  irbase = trace_ir_acq(T);
  /*
  ** Published trace bodies are compact: IR, snapshots and snapmap live inside
  ** the same allocation immediately after GCtrace. Retired bodies can be seen
  ** through stale list entries until SMR completes, so do not trust derived
  ** payload pointers unless they still match the compact layout.
  */
  if (!irbase || &irbase[nk] != (IRIns *)p)
    return 0;
  p += (MSize)(nins - nk) * sizeof(IRIns);
  snap = trace_snap_acq(T);
  if (snap != (SnapShot *)p)
    return 0;
  p += (MSize)nsnap * sizeof(SnapShot);
  snapmap = trace_snapmap_acq(T);
  if (snapmap != (SnapEntry *)p)
    return 0;
  p += nsnapmap * sizeof(SnapEntry);
  if (p != (char *)T + size)
    return 0;
  if (nsnapp)
    *nsnapp = nsnap;
  return 1;
}

/*
** Insert an embedded trace body while owning the sole recorder token. GC uses
** the same one-shot, nonwaiting token protocol as the recorder, so reclaimer
** detach/free and list insertion are serialized and no containment scan or
** same-node help protocol can create a duplicate/cycle.
*/
static void trace_retired_publish_token(jit_State *J, GCtrace *T)
{
  uintptr_t link = trace_retired_link_acq(T);
  lj_assertJ(lj_jit_token_held(J),
	     "trace retire-list insertion without recorder token");
  if (link & TRACE_RETIRED_LINK_LISTED)
    return;
  lj_assertJ(link == TRACE_RETIRED_LINK_UNLINKED,
	     "trace retire-list node has incomplete link state");
  for (;;) {
    GCtrace *head = trace_retired_head_acq(J);
    trace_retired_link_rel(T, (uintptr_t)head | TRACE_RETIRED_LINK_LISTED);
    if (trace_retired_head_cas(J, &head, T))
      break;
    /* Only a token protocol violation can change this head. Rebuild the
    ** unpublished descriptor defensively in release builds and retry.
    */
    lj_assertJ(0, "concurrent trace retire-list writer");
  }
}

static LJ_AINLINE uint64_t trace_retire_stamp(uint64_t epoch)
{
  /* Reserve zero for a live trace. Saturation is conservative at epoch wrap. */
  return epoch >= UINT64_MAX-1u ? UINT64_MAX : epoch + 1u;
}

static LJ_AINLINE uint64_t trace_retire_epoch_decode(uint64_t stamp)
{
  return stamp == UINT64_MAX ? UINT64_MAX : stamp - 1u;
}

static int trace_retired_payload_grace_active(global_State *g, GCtrace *T)
{
  uint64_t completed_epoch, retire_epoch;
  if (!g || !T)
    return 0;
  retire_epoch = la_load64_acq(&T->retire_epoch);
  if (retire_epoch == 0)
    return 0;
  retire_epoch = trace_retire_epoch_decode(retire_epoch);
  completed_epoch = lj_gc2_retire_epoch(g);
  return completed_epoch < retire_epoch ||
	 completed_epoch - retire_epoch < LJ_FLUSH_EPOCHS;
}

static int trace_retired_needs_payload_preserve(global_State *g, GCtrace *T)
{
  /*
  ** A public slot reservation remains an exit-restore name through its SMR grace
  ** generations. The raw body alone is not enough: snapshot restore and stale
  ** exits may still need the compact IR/snapshot/KGC payload while the slot names
  ** this retired body. Non-public retired bodies only need that payload during
  ** the same grace window.
  */
  if (trace_traceno_acq(T) != 0 || trace_nextroot_acq(T) != 0)
    return 1;
  return trace_retired_payload_grace_active(g, T);
}

static void trace_preservebody_raw(global_State *g, GCtrace *T)
{
  (void)lj_gc2_markmem(g, T);
  if (LJ_UNLIKELY(!trace_body_refs_valid(g, T, NULL)))
    return;
  {
    MCode **exittab = trace_exittab_acq(T);
    if (exittab && !trace_exittab_ismcode(T))
      (void)lj_gc2_markmem(g, exittab);
  }
}

static void trace_preserve_retired_body(global_State *g, GCtrace *T,
					TraceProtoPCState *pcstate)
{
  if (trace_retired_needs_payload_preserve(g, T))
    trace_preservebody(g, T, pcstate);
  else
    trace_preservebody_raw(g, T);
}

static void trace_preserve_retired_body_reclaim(global_State *g, GCtrace *T,
						TraceProtoPCState *pcstate)
{
  if (trace_retired_needs_payload_preserve(g, T))
    trace_preservebody_reclaim(g, T, pcstate);
  else
    trace_preservebody_raw(g, T);
}

static void trace_preserve_retired_publish(global_State *g, GCtrace *T)
{
  trace_preserve_retired_body(g, T, NULL);
}

static void trace_retired_push_preserved(jit_State *J, GCtrace *T)
{
  global_State *g = J2G(J);
  /*
  ** Retired trace bodies can be named by stale patched bytecode, exit restore,
  ** or mcode until their SMR epoch completes. Preserve before and after list
  ** publication for the same race covered by tracevec_retire(): active sweep
  ** may already be running, or a root scan may begin between the local mark and
  ** the CAS push.
  */
  lj_assertJ(lj_jit_token_held(J),
	     "retired trace preservation without recorder token");
  trace_preserve_retired_publish(g, T);
  trace_retired_publish_token(J, T);
  trace_preserve_retired_body(g, T, NULL);
}

/* Requeue from inside the exclusive SMR writer. Entering an ordinary reader
** here would wait for this same writer to finish and deadlock the safepoint
** leader. The writer gate and recorder token already protect every detached
** trace/list body consumed by this path. */
static void trace_retired_push_preserved_reclaim(jit_State *J, GCtrace *T)
{
  global_State *g = J2G(J);
  lj_assertJ(lj_gc2_jit_reclaim_context_acq(g) && lj_jit_token_held(J),
	     "retired trace requeue without exclusive reclaim ownership");
  trace_preserve_retired_body_reclaim(g, T, NULL);
  trace_retired_publish_token(J, T);
  trace_preserve_retired_body_reclaim(g, T, NULL);
}

/*
** The retirement LP is the encoded epoch claim while holding the sole recorder
** token. The same token owner immediately publishes the intrusive retire node
** before releasing ownership; slot or semantic unlink can only follow that
** publication. GC never waits for the token and simply retries its bounded
** root/quarantine item after requesting an asynchronous recorder abort.
*/
static void trace_retire_claim_at_epoch(global_State *g, GCtrace *T,
					uint64_t epoch)
{
  uint64_t expect = 0;
  uint64_t stamp = trace_retire_stamp(epoch);
  /* Preserve in GC2 before the unique entry gate; a root-prune caller can then
  ** unlink only after this helper returns without opening an unpreserved gap.
  */
  trace_preserve_retired_publish(g, T);
  (void)la_cas64(&T->retire_epoch, &expect, stamp, LA_ACQ_REL, LA_ACQ);
}

static int trace_retire_discoverable_acq(jit_State *J, GCtrace *T)
{
  TraceVec *tv;
  TraceNo traceno;
  GCobj *slot;
  /* Once listed, the token-owned retire stack is authoritative even if its
  ** public trace slot has already been disconnected.
  */
  if (trace_retired_link_listed_acq(T))
    return 1;
  traceno = trace_traceno_acq(T);
  if (traceno == 0 && la_load64_acq(&T->retire_epoch) != 0)
    traceno = trace_nextroot_acq(T);  /* Retired public-slot reservation. */
  tv = tracevec_acq(J);
  if (traceno != 0 && tv && (MSize)traceno < tv->sizetrace) {
    slot = gcref_acq(tv->slot[traceno]);
    if (slot == obj2gco(T))
      return 1;
  }
  /* A token owner always lists before clearing its exact slot. Recheck the
  ** list publication after a failed slot snapshot to close that handoff race.
  */
  return trace_retired_link_listed_acq(T);
}

static int trace_has_runnable_inbound_link(jit_State *J, GCtrace *target)
{
  TraceVec *tv = tracevec_acq(J);
  TraceNo targetno = trace_traceno_acq(target);
  TraceNo i;
  if (targetno == 0 && la_load64_acq(&target->retire_epoch) != 0)
    targetno = trace_nextroot_acq(target);
  if (targetno == 0 || tv == NULL)
    return 0;
  /*
  ** The recorder token serializes the assembler's validate->publish window and
  ** every semantic trace-link rewrite. Thus a single reverse scan is the
  ** retirement admission check: once it reports no runnable terminal source,
  ** no new persistent mcode edge can appear before the target's epoch claim and
  ** disconnect transaction complete.
  **
  ** A runnable but otherwise unreachable source may conservatively rescue its
  ** target for one collection. The source is retired later in the same root
  ** pass and the target becomes collectible in the next cycle. This bounded
  ** retention is required because root-spine order does not tell admission
  ** whether the source is semantically live; retiring the target first would
  ** leave an unretargetable native edge and an immortal deferred arena cell.
  */
  for (i = 1; (MSize)i < tv->sizetrace; i++) {
    GCtrace *T = traceref_fromgco(gcref_acq(tv->slot[i]));
    if (T && T != target && trace_runnable_acq(T, i) &&
	trace_link_acq(T) == targetno)
      return 1;
  }
  return 0;
}

static void trace_rescue_runnable_target(global_State *g, GCtrace *T)
{
  GCobj *o = obj2gco(T);
  /*
  ** The reverse edge is semantic reachability, not stale-reader body
  ** retention. Reopen the GC2 traversal frontier before declining retirement.
  ** During sweep this publishes through the current TG's SSB; during an earlier
  ** phase the same helper performs an ordinary GC2 semantic mark. The root-prune
  ** cursor then retries the same body and observes its new mark instead of
  ** transferring it to RETIRED.
  */
  (void)lj_gc2_trace_sweep_root(g, o);
}

static void trace_retire_at_epoch(global_State *g, GCtrace *T,
				  uint64_t epoch);

int LJ_FASTCALL lj_trace_retire_gc_claim(global_State *g, GCtrace *T)
{
  jit_State *J;
  int token = 0;
  int discoverable;
  if (!g || !T)
    return 0;
  J = G2J(g);
  if (la_load64_acq(&T->retire_epoch) != 0 &&
      trace_retire_discoverable_acq(J, T))
    return 1;
  /* Recorder/assembler target validation and final direct-link publication are
  ** token-private, but an epoch-only GC claim would otherwise race between
  ** those two operations and retire a target newly published into machine code.
  ** Take the token once without waiting, publish the retire node before release,
  ** and let the bounded root/quarantine pass retry if a recorder won. Abort that
  ** recorder asynchronously so continuous compilation cannot starve sweep.
  */
  if (!lj_jit_token_held(J)) {
    if (!lj_jit_token_try(J)) {
      lj_trace_state_abort(J);
      return 0;
    }
    token = 1;
  }
  /* A terminal link is a persistent native inbound edge. It cannot be
  ** retargeted by disconnecting the target, so admit retirement only after the
  ** token-protected reverse scan proves that no runnable source still names it.
  */
  if (trace_has_runnable_inbound_link(J, T)) {
    trace_rescue_runnable_target(g, T);
    if (token)
      lj_jit_token_release(J);
    return 0;
  }
  trace_retire_at_epoch(g, T, lj_gc2_retire_epoch(g));
  trace_retire_disconnect(J, T);
  discoverable = trace_retire_discoverable_acq(J, T);
  if (token)
    lj_jit_token_release(J);
  return discoverable;
}

static void trace_retire_at_epoch(global_State *g, GCtrace *T, uint64_t epoch)
{
  jit_State *J = G2J(g);
  lj_assertJ(lj_jit_token_held(J),
	     "trace retire-list publication without recorder token");
  trace_retire_claim_at_epoch(g, T, epoch);
  trace_retired_push_preserved(J, T);
}

static void trace_retire(global_State *g, GCtrace *T)
{
  trace_retire_at_epoch(g, T, lj_gc2_retire_epoch(g));
}

static int trace_freebody(global_State *g, GCtrace *T)
{
  GCSize size;
  SnapNo nsnap;
  if (LJ_UNLIKELY(!trace_size_checked(g, T, &size, &nsnap)))
    return 0;
  /* Runtime reclaim only reaches here after the nonwaiting debugger unregister
  ** succeeded. At VM close, lj_trace_freeretired() performs the exceptional
  ** teardown unregister before entering this helper.
  */
#ifdef LUAJIT_USE_GDBJIT
  lj_assertG(trace_gdbjit_entry_acq(T) == NULL,
	     "trace freed with live GDB JIT descriptor");
#endif
  trace_exittab_free(g, T, nsnap);
  /* This release is the exact physical-destructor completion signal consumed
  ** by bounded arena quarantine. The trace retire list is the sole owner of
  ** payload/exittab destruction; a quarantined arena retains the allocation
  ** bitmap until it observes this byte with acquire ordering.
  */
  la_store8_rel(&T->gct, 0);
  lj_mem_free(g, T, size);
  return 1;
}

int LJ_FASTCALL lj_trace_body_destroyed_acq(const GCtrace *T)
{
  return T && la_load8_acq(&T->gct) == 0;
}

static void trace_free_immediate(global_State *g, GCtrace *T)
{
  GCSize size;
  SnapNo nsnap;
  if (LJ_UNLIKELY(!trace_size_checked(g, T, &size, &nsnap)))
    return;
  trace_exittab_free(g, T, nsnap);
  T->gct = 0;  /* Unpublished aborts may race with preserved retired scans. */
  lj_mem_free(g, T, size);
}

void LJ_FASTCALL lj_trace_free_unpublished(global_State *g, GCtrace *T)
{
  jit_State *J = G2J(g);
  /* GC2 may have acquired J->curfinal immediately before the owner replaces or
  ** aborts it. Use the ordinary epoch/SMR retire path even though no trace slot
  ** was published; this keeps the compact body immutable until that reader exits.
  ** Unlike a public GC claim, this body has no discovery slot, so its recorder
  ** owner must insert it while retaining the token.
  */
  lj_assertJ(lj_jit_token_held(J),
	     "unpublished trace retirement without recorder token");
  trace_retire(g, T);
}

static LJ_AINLINE int trace_body_retire_ready(GCtrace *T,
					       uint64_t completed_epoch)
{
  uint64_t stamp = la_load64_acq(&T->retire_epoch);
  uint64_t retire_epoch;
  if (stamp == 0)
    return 0;
  retire_epoch = trace_retire_epoch_decode(stamp);
  return completed_epoch >= retire_epoch &&
	 completed_epoch - retire_epoch >= LJ_FLUSH_EPOCHS;
}

static int trace_preserve_body_candidate(global_State *g, GCobj *o,
					 uint32_t *gctp)
{
  uint32_t gct;
  if (!lj_gc2_obj_valid_queued(g, o))
    return 0;
  gct = (uint32_t)la_load8_acq(&o->gch.gct);
  if (LJ_UNLIKELY(gct == 0 || gct < (uint32_t)~LJ_TSTR ||
		  gct > (uint32_t)~LJ_TUDATA))
    return 0;
  if (gctp)
    *gctp = gct;
  return 1;
}

static void trace_preserve_body_obj(global_State *g, GCobj *o)
{
  if (gc2_phase_acq(g) == LJ_GC2_IDLE)
    return;  /* No reclaim can race; active root scans republish this edge. */
  /* Retired KGC operands share the semantic arena mark domain. Queue their
  ** graph so this body pin cannot poison later ordinary marking. Retain first:
  ** the retired trace lease protects T/IR, not the child allocation. FINREG is
  ** allowed to observe the resulting physical cdata pin independently. */
  (void)lj_gc2_markobj(g, o);
}

static void trace_preserve_proto_obj(global_State *g, GCobj *o)
{
  uint32_t gct;
  if (gc2_phase_acq(g) == LJ_GC2_IDLE)
    return;  /* The retired body/list remains discoverable at activation. */
  /* Retired traces need prototype bodies for stale PC ownership checks. */
  if (lj_gc2_markobj_status(g, o, &gct) < 0 ||
      gct != (uint32_t)~LJ_TPROTO)
    return;
  /* The semantic mark plane cannot encode "body only". Queue traversal when
  ** active so a proto body pin never hides its graph from a later root. Trace
  ** unlink still owns the lifetime of the retired proto->trace edge. */
}

#define TRACE_PROTO_PC_CACHE	256
#define TRACE_PROTO_PC_WALK_BUDGET	8192u

typedef struct TraceProtoPCCache {
  const BCIns *start;
  const BCIns *end;
  GCobj *o;
} TraceProtoPCCache;

struct TraceProtoPCState {
  TraceProtoPCCache cache[TRACE_PROTO_PC_CACHE];
  MSize ncache;
  uint32_t walk_budget;
};

static void trace_proto_pc_state_init(TraceProtoPCState *pcstate)
{
  pcstate->ncache = 0;
  pcstate->walk_budget = TRACE_PROTO_PC_WALK_BUDGET;
}

static GCproto *trace_proto_pc_candidate(global_State *g, GCobj *o,
					 const BCIns **bcp,
					 const BCIns **endp)
{
  GCproto *pt;
  const BCIns *bc;
  uint32_t gct;
  if (!trace_preserve_body_candidate(g, o, &gct) ||
      gct != (uint32_t)~LJ_TPROTO)
    return NULL;
  pt = gco2pt(o);
  if (!lj_gc2_valid_proto_for_traverse(g, pt))
    return NULL;
  bc = proto_bc(pt);
  if (bcp)
    *bcp = bc;
  if (endp)
    *endp = bc + pt->sizebc;
  return pt;
}

#ifdef LJ_TRACE_TEST_HELPERS
int lj_trace_test_preserve_body_candidate(global_State *g, GCobj *o)
{
  return trace_preserve_body_candidate(g, o, NULL);
}

int lj_trace_test_proto_pc_candidate(global_State *g, GCobj *o,
				     const BCIns *pc)
{
  const BCIns *bc, *end;
  return pc && trace_proto_pc_candidate(g, o, &bc, &end) &&
	 pc >= bc && pc < end;
}
#endif

static void trace_preserve_proto_for_pc(global_State *g, const BCIns *pc,
					TraceProtoPCCache *cache,
					MSize *ncachep, uint32_t *budgetp)
{
  GCobj *o;
  MSize i, ncache = *ncachep;
  if (!pc || !budgetp || *budgetp == 0)
    return;
  for (i = 0; i < ncache; i++) {
    if (pc >= cache[i].start && pc < cache[i].end) {
      trace_preserve_proto_obj(g, cache[i].o);
      return;
    }
  }
  (void)lj_gc_flush_root_pending(g);
  (void)lj_gc_repair_root_spine(g);
  for (o = lj_gc_root_acq(g); o != NULL && *budgetp != 0;) {
    GCobj *next;
    if (!lj_gc2_obj_valid(g, o))
      break;
    next = lj_obj_gcw_acq(o);
    --*budgetp;
    {
      const BCIns *bc, *end;
      GCproto *pt = trace_proto_pc_candidate(g, o, &bc, &end);
      if (pt) {
	/*
	** Snapshot PC preservation often checks many PCs from the same generation
	** of protos. Populate the bounded interval cache while walking the root
	** spine, not only when the current PC matches, so later snapshot entries do
	** not repeat the same ownership walk.
	*/
	if (ncache < TRACE_PROTO_PC_CACHE) {
	  cache[ncache].start = bc;
	  cache[ncache].end = end;
	  cache[ncache].o = o;
	  *ncachep = ++ncache;
	}
	if (pc >= bc && pc < end) {
	  trace_preserve_proto_obj(g, o);
	  return;
	}
      }
    }
    o = next;
    /*
    ** Retired traces preserve snapshot PCs during GC2 handshakes. This is
    ** advisory body retention, not semantic reachability, so the whole trace
    ** preservation pass shares a bounded root-spine budget. A trace with many
    ** snapshot PCs must not repeat a long ownership-spine walk at every PC.
    */
  }
}

static void trace_preserve_snapshot_pcs(global_State *g, GCtrace *T,
					TraceProtoPCState *pcstate)
{
  SnapShot *snap = trace_snap_acq(T);
  SnapEntry *snapmap = trace_snapmap_acq(T);
  MSize nsnapmap = trace_nsnapmap_acq(T);
  SnapNo i, nsnap = trace_nsnap_acq(T);
  TraceProtoPCState localstate;
  if (!snap || !snapmap)
    return;
  if (!pcstate) {
    trace_proto_pc_state_init(&localstate);
    pcstate = &localstate;
  }
  (void)lj_gc_flush_root_pending(g);
  for (i = 0; i < nsnap; i++) {
    SnapShot *s = &snap[i];
    MSize ofs = snap_mapofs_acq(s);
    MSize nent = snap_nent_acq(s);
    SnapEntry *map;
    if (ofs >= nsnapmap || nent >= nsnapmap - ofs)
      return;
    map = &snapmap[ofs];
    trace_preserve_proto_for_pc(g, snap_pc_acq(&map[nent]),
				pcstate->cache, &pcstate->ncache,
				&pcstate->walk_budget);
  }
}

static void trace_preserve_kgc(global_State *g, GCtrace *T)
{
  IRIns *irbase = trace_ir_acq(T);
  IRRef ref;
  if (!irbase)
    return;
  for (ref = trace_nk_acq(T); ref < REF_TRUE; ref++) {
    IRIns *ir = &irbase[ref];
    IRIns irs = ir_load_acq(ir);
    if (irs.o == IR_KGC) {
      GCobj *o = ir_kgc_load_acq(ir);
      trace_preserve_body_obj(g, o);
    }
    if (irt_is64(irs.t) && irs.o != IR_KNULL)
      ref++;
  }
}

static void trace_preservebody_inner(global_State *g, GCtrace *T,
				     TraceProtoPCState *pcstate)
{
  /* Retired traces are SMR-protected bodies, but stale bytecode readers can
  ** still redispatch through their start/snapshot PCs until the grace period
  ** completes. Preserve those prototype owners along with the body and
  ** auxiliary exit table. The compact body can also contain GC operands that
  ** stale machine-code readers/snapshot restorers may still load, so preserve
  ** those object bodies without treating the retired trace as a live trace slot.
  ** Published live traces mark the same graph through traversal.
  */
  (void)lj_gc2_markmem(g, T);
  if (LJ_UNLIKELY(!trace_body_refs_valid(g, T, NULL)))
    return;
  trace_preserve_kgc(g, T);
  trace_preserve_proto_obj(g, trace_startptgco_acq(T));
  trace_preserve_snapshot_pcs(g, T, pcstate);
  /* A retire publisher can overlap the last semantic traversal that would have
  ** followed these direct trace links. Raw-marking T must not make that later
  ** traversal see ALREADY while silently dropping the targets. */
  lj_gc2_mark_trace_slot(g, trace_link_acq(T));
  lj_gc2_mark_trace_slot(g, trace_nextroot_acq(T));
  lj_gc2_mark_trace_slot(g, trace_nextside_acq(T));
  {
    MCode **exittab = trace_exittab_acq(T);
    if (exittab && !trace_exittab_ismcode(T))
      (void)lj_gc2_markmem(g, exittab);
  }
}

static void trace_preservebody(global_State *g, GCtrace *T,
			       TraceProtoPCState *pcstate)
{
  lj_gc2_smr_read_enter(g);
  trace_preservebody_inner(g, T, pcstate);
  lj_gc2_smr_read_leave(g);
}

static void trace_preservebody_reclaim(global_State *g, GCtrace *T,
				       TraceProtoPCState *pcstate)
{
  jit_State *J = G2J(g);
  lj_assertJ(lj_gc2_jit_reclaim_context_acq(g) && lj_jit_token_held(J),
	     "trace body preservation without exclusive reclaim ownership");
  trace_preservebody_inner(g, T, pcstate);
}

static int trace_retired_slot_clear(jit_State *J, TraceVec *tv, TraceNo traceno,
				    GCtrace *T)
{
  GCobj *slot;
  if (traceno == 0 || tv == NULL || (MSize)traceno >= tv->sizetrace)
    return 0;
  slot = gcref_acq(tv->slot[traceno]);
  if (slot == obj2gco(T)) {
    setgcrefrel(tv->slot[traceno], NULL);
    if (J->freetrace == 0 || traceno < J->freetrace)
      J->freetrace = traceno;
    return 1;
  }
  return 0;
}

static void trace_retired_slot_release(jit_State *J, GCtrace *T)
{
  TraceNo traceno = trace_traceno_acq(T);
  int cleared = 0;
  TraceVec *tv;
  lj_assertJ(lj_jit_token_held(J),
	     "trace slot release without recorder-token ownership");
  if (traceno == 0 && la_load64_acq(&T->retire_epoch) != 0)
    traceno = trace_nextroot_acq(T);
  tv = tracevec_acq(J);
  cleared = trace_retired_slot_clear(J, tv, traceno, T);
  if (!cleared && tv) {
    TraceNo i;
    /*
    ** Retired traces normally reserve their public slot number in nextroot.
    ** Reclaim runs outside the recorder token, while trace-vector growth and
    ** root-chain unlinking are independent publications. If that compact
    ** metadata no longer names the current slot, release by exact body identity
    ** before the body is freed; this is a cold SMR cleanup path, not a semantic
    ** trace root walk.
    */
    for (i = 1; i < tv->sizetrace; i++) {
      if (i != traceno && trace_retired_slot_clear(J, tv, i, T)) {
	traceno = i;
	cleared = 1;
	break;
      }
    }
  }
  trace_test_note_slot_release(traceno, cleared);
  trace_nextroot_rel(T, 0);
  trace_traceno_rel(T, 0);
}

static void trace_slot_retire(jit_State *J, GCtrace *T, TraceNo traceno)
{
  global_State *g = J2G(J);
  lj_assertJ(lj_jit_token_held(J),
	     "trace slot retirement without recorder-token ownership");
  if (mt_active_or_entering_acq(g) || gc2_n_threads_acq(g) > 1) {
    /*
    ** Keep the public trace slot reserved with the retired body until stale
    ** bytecode readers and in-flight exits have aged out. T->traceno and
    ** retire_epoch remain the runnable gate, so VM/recorder/assembler entry
    ** paths reject this body, but snapshot restore can still resolve the
    ** exiting trace after its live root/side links have been unlinked. Sticky MT
    ** mode keeps the same reservation between worker generations, but only until
    ** LJ_FLUSH_EPOCHS completed safepoint generations prove that every stale
    ** reader has quiesced. While retire_epoch is non-zero, nextroot is private
    ** slot-reservation metadata; normal root chains were unlinked before retiring
    ** the trace.
    */
    trace_nextroot_rel(T, traceno);
    trace_traceno_rel(T, 0);
    traceslot_publish(J, traceno, T);
  } else {
    trace_retired_slot_release(J, T);
  }
}

/* Complete the JIT-state half of a previously published retirement claim.
** GC2 may publish the claim while another TG records, but only the token
** owner may disconnect the public slot or update J->freetrace. */
static int trace_finish_slot_retire(jit_State *J, GCtrace *T)
{
  TraceNo traceno;
  int debug_done;
  lj_assertJ(lj_jit_token_held(J),
	     "trace destructor without recorder-token ownership");
  /* Optional GDB descriptor mutation is process-global. It is deliberately
  ** nonwaiting; the body stays on the retire list and a later grace pass retries
  ** before physical free if another universe currently owns that descriptor.
  */
  debug_done = lj_gdbjit_deltrace(J, T);
  traceno = trace_traceno_acq(T);
  if (traceno != 0) {
    trace_slot_retire(J, T, traceno);
  }
  return debug_done;
}

static LJ_AINLINE int trace_exit_body_match(const GCtrace *T, TraceNo traceno)
{
  if (T == NULL || traceno == 0)
    return 0;
  if (trace_traceno_acq(T) == traceno)
    return 1;
  /*
  ** Retired traces stay published in their public slot until the SMR epoch
  ** expires. They are not runnable because traceno is cleared, but in-flight
  ** machine-code exits may still carry the public trace number and need the
  ** preserved snapshot body to restore interpreter state.
  */
  return trace_traceno_acq(T) == 0 &&
	 la_load64_acq(&T->retire_epoch) != 0 &&
	 trace_nextroot_acq(T) == traceno;
}

uint32_t lj_trace_reclaim_retired(global_State *g, uint64_t completed_epoch)
{
  jit_State *J;
  TraceVec *tv;
  GCtrace *rt;
  uint32_t reclaimed = 0;
  int token = 0;
  if (!g || completed_epoch == 0)
    return 0;
  J = G2J(g);
  /*
  ** Slot release updates J->freetrace and must serialize with the sole recorder.
  ** Reclamation is opportunistic: never wait for a peer that owns the token,
  ** because that recorder may itself be waiting for the outer SMR reclaim pass to
  ** finish. The caller will retry at a later completed epoch.
  */
  if (!lj_jit_token_held(J)) {
    if (!lj_jit_token_try(J))
      return 0;
    token = 1;
  }
  if (!lj_gc2_jit_reclaim_context_acq(g) || !lj_jit_token_held(J)) {
    if (token)
      lj_jit_token_release(J);
    return 0;
  }
  lj_assertG(lj_gc2_jit_reclaim_context_acq(g) && lj_jit_token_held(J),
	     "trace retire-list detach without exclusive reclaim gate");
  tv = tracevec_retired_head_xchg_acqrel(J, NULL);
  while (tv && lj_gc2_mem_registered(g, tv)) {
    TraceVec *next = tracevec_retired_next_acq(tv);
    tracevec_retired_next_rel(tv, NULL);
    if (la_load64_acq(&tv->retire_epoch) < completed_epoch) {
      tracevec_free(g, tv);
      reclaimed++;
    } else {
      tracevec_retired_push(J, tv);
    }
    tv = next;
  }
  rt = trace_retired_head_xchg_acqrel(J, NULL);
  while (rt && lj_gc2_mem_registered(g, rt)) {
    GCtrace *next = trace_retired_next_acq(rt);
    trace_retired_link_unlinked_rel(rt);
    if (trace_body_retire_ready(rt, completed_epoch)) {
      if (trace_has_runnable_inbound_link(J, rt)) {
	trace_retired_push_preserved_reclaim(J, rt);
	rt = next;
	continue;
      }
      /*
      ** The completed generation and the outer zero-reader SMR gate jointly prove
      ** that no stale bytecode entry or native exit can still require this public
      ** trace number. Clear the exact body identity before making the number free;
      ** a replacement trace may publish the same number immediately afterward.
      */
      if (!trace_finish_slot_retire(J, rt)) {
	trace_retired_push_preserved_reclaim(J, rt);
	rt = next;
	continue;
      }
      if (trace_traceno_acq(rt) != 0 || trace_nextroot_acq(rt) != 0)
	trace_retired_slot_release(J, rt);
      /* The sweep-owner reclaim gate runs only after its bounded bridge/root
      ** prune detached every old object. Avoid a redundant unbounded spine walk
      ** while the world is otherwise ready for bounded quarantine progress.
      ** IDLE reclaim still needs the compatibility unlink for traces retired by
      ** an ordinary jit.flush() before another sweep has detached their root.
      */
      if (gc2_phase_acq(g) != LJ_GC2_SWEEP)
	lj_gc_unlink_root_obj(g, obj2gco(rt));
      if (!trace_freebody(g, rt)) {
	/* A malformed/stale compact header is not an allocator contract. Keep the
	** exact body and its mcode references discoverable instead of turning a
	** validation failure into an unbounded-size free or executable UAF.
	*/
	trace_retired_push_preserved_reclaim(J, rt);
	rt = next;
	continue;
      }
      reclaimed++;
    } else {
      trace_retired_push_preserved_reclaim(J, rt);
    }
    rt = next;
  }
  if (token)
    lj_jit_token_release(J);
  return reclaimed;
}

static int trace_ptr_in_mcode_span(const void *ptr, uintptr_t lo, uintptr_t hi)
{
  uintptr_t p = (uintptr_t)ptr;
  return p >= lo && p < hi;
}

static int trace_ptr_in_mcode_area(const void *ptr, uintptr_t rxlo,
				   uintptr_t rxhi, uintptr_t rwlo,
				   uintptr_t rwhi)
{
  return trace_ptr_in_mcode_span(ptr, rxlo, rxhi) ||
	 trace_ptr_in_mcode_span(ptr, rwlo, rwhi);
}

static int trace_mcode_area_refs(global_State *g, GCtrace *T, uintptr_t rxlo,
				 uintptr_t rxhi, uintptr_t rwlo,
				 uintptr_t rwhi)
{
  MCode *mcode;
  MCode *exitstub;
  MCode **exittab;
  SnapNo i, nsnap;
  if (!T)
    return 0;
  if (LJ_UNLIKELY(!trace_body_refs_valid(g, T, &nsnap)))
    return 1;  /* Keep mcode if a stale retired body cannot be decoded safely. */
  mcode = trace_mcode_acq(T);
  exitstub = trace_exitstub_acq(T);
  exittab = trace_exittab_acq(T);
  if (trace_ptr_in_mcode_area(mcode, rxlo, rxhi, rwlo, rwhi) ||
      trace_ptr_in_mcode_area(exitstub, rxlo, rxhi, rwlo, rwhi))
    return 1;
  if (!exittab)
    return 0;
  if (trace_exittab_ismcode(T))
    return trace_ptr_in_mcode_area(exittab, rxlo, rxhi, rwlo, rwhi);
  for (i = 0; i < nsnap; i++)
    if (trace_ptr_in_mcode_area(la_loadptr_acq((void *const *)&exittab[i]),
				rxlo, rxhi, rwlo, rwhi))
      return 1;
  return 0;
}

int lj_trace_retired_mcode_refs(global_State *g, MCode *area, size_t size)
{
  jit_State *J;
  GCtrace *T;
  MCode *rwarea;
  uintptr_t rxlo, rxhi, rwlo, rwhi;
  if (!g || !area || size == 0)
    return 0;
  J = G2J(g);
  rwarea = lj_mcode_area_rw(area);
  rxlo = (uintptr_t)area;
  rxhi = rxlo + size;
  rwlo = (uintptr_t)rwarea;
  rwhi = rwlo + size;
  {
    TraceNo i, sizetrace = trace_sizetrace_acq(J);
    for (i = 1; i < sizetrace; i++)
      if (trace_mcode_area_refs(g, traceref_safe(J, i),
				rxlo, rxhi, rwlo, rwhi))
	return 1;
  }
  for (T = trace_retired_head_acq(J);
       T != NULL && lj_gc2_mem_registered(g, T);
       T = trace_retired_next_acq(T)) {
    if (trace_mcode_area_refs(g, T, rxlo, rxhi, rwlo, rwhi))
      return 1;
  }
  return 0;
}

void lj_trace_freeretired(global_State *g)
{
  jit_State *J = G2J(g);
  TraceVec *tv = tracevec_retired_head_xchg_acqrel(J, NULL);
  GCtrace *rt;
  while (tv && lj_gc2_mem_registered(g, tv)) {
    TraceVec *next = tracevec_retired_next_acq(tv);
    tracevec_free(g, tv);
    tv = next;
  }
  rt = trace_retired_head_xchg_acqrel(J, NULL);
  while (rt && lj_gc2_mem_registered(g, rt)) {
    GCtrace *next = trace_retired_next_acq(rt);
    int freed;
    /* lj_trace_freestate() has already verified and freed the active trace
    ** vector. Do not route close-time body draining through the runtime slot
    ** release helper: there is no slot left to clear and no token-free write to
    ** J->freetrace is permitted. */
    lj_gdbjit_deltrace_close(g, rt);
    freed = trace_freebody(g, rt);
    lj_assertG(freed, "invalid retired trace body at VM close");
    UNUSED(freed);
    rt = next;
  }
}

void lj_trace_markvecs(global_State *g, int gc2)
{
  jit_State *J = G2J(g);
  TraceVec *tv = tracevec_acq(J);
  GCtrace *rt;
  TraceProtoPCState pcstate;
  UNUSED(gc2);  /* Compatibility argument: GC2 is the sole runtime collector. */
  trace_proto_pc_state_init(&pcstate);
  if (tv) {
    (void)lj_gc2_markmem(g, tv);
    /*
    ** The trace vector is raw VM metadata and must survive concurrent readers,
    ** but its slots are not semantic roots. Stock LuaJIT lets a dead prototype
    ** and its root traces die together: live prototypes, active TG vmstates and
    ** live trace links mark the needed trace bodies. Marking every published
    ** slot here creates a trace->prototype cycle and keeps throwaway compiled
    ** chunks alive indefinitely.
    */
    {
      TraceNo i, sizetrace = trace_sizetrace_acq(J);
      /*
      ** A nonzero encoded retirement claim gates new entry before semantic links
      ** are removed. Preserve any such slot body; ordinary unretired live traces
      ** still derive reachability from prototypes, links and TG vmstates.
      */
      for (i = 1; i < sizetrace; i++) {
	GCtrace *T = traceref_safe(J, i);
	if (T && la_load64_acq(&T->retire_epoch) != 0)
	  trace_preserve_retired_body(g, T, &pcstate);
      }
    }
  }
  for (tv = tracevec_retired_head_acq(J);
       tv != NULL && lj_gc2_mem_registered(g, tv);
       tv = tracevec_retired_next_acq(tv)) {
    (void)lj_gc2_markmem_registered(g, tv);
  }
  for (rt = trace_retired_head_acq(J);
       rt != NULL && lj_gc2_mem_registered(g, rt);
       rt = trace_retired_next_acq(rt))
    trace_preserve_retired_body(g, rt, &pcstate);
}

/* Find a free trace number. */
static TraceNo trace_findfree(jit_State *J)
{
  MSize osz, lim;
  TraceVec *oldtv, *newtv;
#ifdef LJ_TRACE_TEST_HELPERS
  (void)la_add32_acqrel(&trace_test_findfree_calls, 1);
#endif
  if (J->freetrace == 0)
    J->freetrace = 1;
  for (; J->freetrace < trace_sizetrace_acq(J); J->freetrace++)
    if (gcref_acq(*traceslot_ref_acq(J, J->freetrace)) == NULL) {
      trace_test_note_findfree_reuse(J->freetrace);
      return J->freetrace++;
    }
  /* Need to grow trace array. */
  lim = (MSize)jit_param_acq(J, JIT_P_maxtrace) + 1;
  if (lim < 2) lim = 2; else if (lim > 65535) lim = 65535;
  osz = trace_sizetrace_acq(J);
  if (osz >= lim)
    return 0;  /* Too many traces. */
  newtv = tracevec_new(J->L, lim);
  /*
  ** Trace-body reclaim clears public slots and frees retired bodies after the
  ** SMR grace period. Hold an SMR read section while copying and publishing the
  ** replacement vector so reclaim cannot clear/free a copied retired slot before
  ** the new vector becomes the current publication.
  */
  lj_gc2_smr_read_enter(J2G(J));
  oldtv = tracevec_acq(J);
  osz = trace_sizetrace_acq(J);
  if (oldtv)
    memcpy(newtv->slot, oldtv->slot, osz*sizeof(GCRef));
  tracevec_publish(J, newtv);
  tracevec_retire(J, oldtv);
  lj_gc2_smr_read_leave(J2G(J));
  trace_test_note_findfree_grow(J->freetrace);
  return J->freetrace;
}

#define TRACE_APPENDVEC(field, szfield, tp) \
  T->field = (tp *)p; \
  memcpy(p, J->cur.field, J->cur.szfield*sizeof(tp)); \
  p += J->cur.szfield*sizeof(tp);

#ifdef LUAJIT_USE_PERFTOOLS
/*
** Create symbol table of JIT-compiled code. For use with Linux perf tools.
** Example usage:
**   perf record -f -e cycles luajit test.lua
**   perf report -s symbol
**   rm perf.data /tmp/perf-*.map
*/
#include <stdio.h>
#include <unistd.h>

static FILE *perftools_native_fopen(lua_State *L, const char *fname)
{
  FILE *fp;
  lj_native_enter(L2TG(L));
  fp = fopen(fname, "w");
  (void)lj_native_leave(L);
  return fp;
}

static void perftools_native_fprintf(lua_State *L, FILE *fp, uintptr_t mcode,
				     MSize szmcode, TraceNo traceno,
				     const char *name, BCLine lineno)
{
  lj_native_enter(L2TG(L));
  fprintf(fp, "%lx %x TRACE_%d::%s:%u\n",
	  (long)mcode, szmcode, traceno, name, lineno);
  (void)lj_native_leave(L);
}

static void perftools_addtrace(jit_State *J, GCtrace *T)
{
  static FILE *fp;
  lua_State *L = J->L;
  GCproto *pt = trace_startpt_acq(T);
  const BCIns *startpc = trace_startpc_acq(T);
  MCode *mcode = trace_mcode_acq(T);
  MSize szmcode = trace_szmcode_acq(T);
  TraceNo traceno = trace_traceno_acq(T);
  const char *name = proto_chunknamestr_acq(pt);
  BCLine lineno;
  if (name[0] == '@' || name[0] == '=')
    name++;
  else
    name = "(string)";
  lj_assertX(startpc >= proto_bc(pt) && startpc < proto_bc(pt) + pt->sizebc,
	     "trace PC out of range");
  lineno = lj_debug_line(pt, proto_bcpos(pt, startpc));
  if (!fp) {
    char fname[40];
    sprintf(fname, "/tmp/perf-%d.map", getpid());
    if (!(fp = perftools_native_fopen(L, fname))) return;
    setlinebuf(fp);
  }
  perftools_native_fprintf(L, fp, (uintptr_t)mcode, szmcode, traceno,
			   name, lineno);
}
#endif

/* Allocate space for copy of T. */
GCtrace * LJ_FASTCALL lj_trace_alloc(lua_State *L, GCtrace *T)
{
  size_t sztr = ((sizeof(GCtrace)+7)&~7);
  size_t szins = (T->nins-T->nk)*sizeof(IRIns);
  size_t sz = sztr + szins +
	      T->nsnap*sizeof(SnapShot) +
	      T->nsnapmap*sizeof(SnapEntry);
  GCtrace *T2 = (GCtrace *)lj_mem_newgco_raw(L, (MSize)sz,
					     LJ_AF_TRAVERSABLE);
  char *p = (char *)T2 + sztr;
  T2->gct = ~LJ_TTRACE;
  lj_obj_setgcflags(obj2gco(T2), 0);
  T2->traceno = 0;
  T2->ir = (IRIns *)p - T->nk;
  T2->nins = T->nins;
  T2->nk = T->nk;
  T2->nsnap = T->nsnap;
  T2->nsnapmap = T->nsnapmap;
  trace_startpt_clear(T2);
  setmref(T2->startpc, NULL);
  T2->startins = 0;
  T2->szmcode = 0;
  T2->mcode = NULL;
  T2->exittab = NULL;
  T2->exitstub = NULL;
  T2->mcloop = 0;
  T2->nchild = 0;
  T2->spadjust = 0;
  trace_link_rel(T2, 0);
  T2->root = 0;
  trace_nextroot_rel(T2, 0);
  trace_nextside_rel(T2, 0);
  T2->sinktags = 0;
  T2->topslot = 0;
  T2->linktype = 0;
  T2->unused1 = 0;
  T2->retire_epoch = 0;
  trace_retired_link_unlinked_rel(T2);
  memcpy(p, T->ir + T->nk, szins);
  return T2;
}

static void trace_exittab_free(global_State *g, GCtrace *T, SnapNo nsnap)
{
  MCode **exittab = trace_exittab_acq(T);
  if (exittab) {
    trace_exittab_rel(T, NULL);
    if (!trace_exittab_ismcode(T))
      lj_mem_freevec(g, exittab, nsnap, MCode *);
  }
  trace_exittab_mcode_clear(T);
  trace_exitstub_rel(T, NULL);
}

static void trace_exittab_reset(jit_State *J, GCtrace *T)
{
#if LJ_64 && defined(EXITSTUBS_PER_GROUP)
  ExitNo i;
  if (trace_exittab_acq(T) == NULL)
    return;
  for (i = 0; i < trace_nsnap_acq(T); i++)
    trace_exittarget_rel(T, i, exitstub_addr(J, i));
#else
  UNUSED(J); UNUSED(T);
#endif
}

static void trace_exittab_resetroot(jit_State *J, TraceNo rootno)
{
  global_State *g = J2G(J);
  TraceNo i;
  MSize sizetrace = trace_sizetrace_acq(J);
  lj_gc2_smr_read_enter(g);
  for (i = 1; i < sizetrace; i++) {
    GCtrace *T = traceref_safe(J, i);
    if (T && (trace_traceno_acq(T) == rootno || trace_root_acq(T) == rootno))
      trace_exittab_reset(J, T);
  }
  lj_gc2_smr_read_leave(g);
}

/* Save current trace by copying and compacting it. */
static void trace_save(jit_State *J, GCtrace *T)
{
  size_t sztr = ((sizeof(GCtrace)+7)&~7);
  size_t szins = (J->cur.nins-J->cur.nk)*sizeof(IRIns);
  char *p = (char *)T + sztr;
  global_State *g = J2G(J);
  memcpy(T, &J->cur, sizeof(GCtrace));
  newwhite(g, T);
  T->gct = ~LJ_TTRACE;
  T->retire_epoch = 0;
  trace_retired_link_unlinked_rel(T);
  T->ir = (IRIns *)p - J->cur.nk;  /* The IR has already been copied above. */
#if LJ_ABI_PAUTH
  T->mcauth = lj_ptr_sign((ASMFunction)T->mcode, T);
#endif
  p += szins;
  TRACE_APPENDVEC(snap, nsnap, SnapShot)
  TRACE_APPENDVEC(snapmap, nsnapmap, SnapEntry)
  J->cur.traceno = 0;
  J->cur.exittab = NULL;
  J->cur.exitstub = NULL;
  J->curfinal = NULL;
  lj_gc_linkobj_new(g, obj2gco(T));  /* Publish root after body init. */
  traceslot_publish(J, T->traceno, T);
  lj_gc_pubtrace(g, T->traceno);
  lj_gdbjit_addtrace(J, T);
#ifdef LUAJIT_USE_PERFTOOLS
  perftools_addtrace(J, T);
#endif
}

int LJ_FASTCALL lj_trace_free_gc(global_State *g, GCtrace *T)
{
  jit_State *J = G2J(g);
  TraceNo traceno = trace_traceno_acq(T);
  int sfixed = (g->gc.currentwhite & LJ_GC_SFIXED) != 0;
  int needs_publish;
  int needs_slot;
  int debug_done = 1;
  int token = 0;
  lj_assertG(traceno != 0 || trace_startptgco_acq(T) != NULL ||
	     la_load64_acq(&T->retire_epoch) != 0,
	     "unpublished trace body retired");
  UNUSED(traceno);
  needs_publish = !sfixed && !trace_retired_link_listed_acq(T);
  needs_slot = trace_traceno_acq(T) != 0;
  /* GC workers never wait for an active recorder. Claim/list publication and
  ** slot teardown are one token-serialized transaction; a loser requests an
  ** asynchronous recorder abort and leaves the root/quarantine body unchanged
  ** for the next bounded sweep pass.
  */
  if (needs_publish || needs_slot) {
    if (!lj_jit_token_held(J)) {
      if (sfixed) {
	token = lj_jit_token_acquire_wait(J);
      } else if (!lj_jit_token_try(J)) {
	lj_trace_state_abort(J);
	return 0;
      } else {
	token = 1;
      }
    }
    if (!sfixed) {
      /* Publish before semantic entry/root teardown. Keep both operations in
      ** this token transaction so no assembler can install a new direct edge
      ** after the retirement epoch starts. */
      trace_retire_at_epoch(g, T, lj_gc2_retire_epoch(g));
      trace_retire_disconnect(J, T);
    }
    if (needs_slot)
      debug_done = trace_finish_slot_retire(J, T);
  }

  if (sfixed) {
    if (!debug_done)
      lj_gdbjit_deltrace_close(g, T);
    if (la_load64_acq(&T->retire_epoch) != 0) {
      /* A listed body is owned by lj_trace_freeretired(). Keep the intrusive
      ** link valid until that single close-time drain reaches it.
      */
      if ((!trace_retired_link_listed_acq(T) ||
	   trace_traceno_acq(T) != 0 || trace_nextroot_acq(T) != 0) &&
	  !lj_jit_token_held(J))
	token = lj_jit_token_acquire_wait(J);
      if (!trace_retired_link_listed_acq(T))
	trace_retired_push_preserved(J, T);
      if (trace_traceno_acq(T) != 0 || trace_nextroot_acq(T) != 0)
	trace_retired_slot_release(J, T);
      if (token)
	lj_jit_token_release(J);
      return 1;
    }
    if ((trace_traceno_acq(T) != 0 || trace_nextroot_acq(T) != 0) &&
	!lj_jit_token_held(J))
      token = lj_jit_token_acquire_wait(J);
    if (trace_traceno_acq(T) != 0 || trace_nextroot_acq(T) != 0)
      trace_retired_slot_release(J, T);
    trace_free_immediate(g, T);
    if (token)
      lj_jit_token_release(J);
    return 1;
  }
  if (token)
    lj_jit_token_release(J);
  return 1;
}

void LJ_FASTCALL lj_trace_free(global_State *g, GCtrace *T)
{
  (void)lj_trace_free_gc(g, T);
}

/* Re-enable compiling a prototype by unpatching any modified bytecode. */
void lj_trace_reenableproto(GCproto *pt)
{
  if ((pt->flags & PROTO_ILOOP)) {
    BCIns *bc = proto_bc(pt);
    BCPos i, sizebc = pt->sizebc;
    pt->flags &= ~PROTO_ILOOP;
    if (bc_op(bc[0]) == BC_IFUNCF)
      bc_publish_op(&bc[0], BC_FUNCF);
    for (i = 1; i < sizebc; i++) {
      BCOp op = bc_op(bc[i]);
      if (op == BC_IFORL || op == BC_IITERL || op == BC_ILOOP)
	bc_publish_op(&bc[i], (int)op+(int)BC_LOOP-(int)BC_ILOOP);
    }
  }
}

/* Unpatch the bytecode modified by a root trace. */
static void trace_unpatch(jit_State *J, GCtrace *T)
{
  BCIns startins = trace_startins_acq(T);
  BCOp op = bc_op(startins);
  BCIns *pc = (BCIns *)trace_startpc_acq(T);
  TraceNo traceno = trace_traceno_acq(T);
  BCIns cur;
  if (op == BC_JMP)
    return;  /* No need to unpatch branches in parent traces (yet). */
  cur = (BCIns)la_load32_acq((uint32_t *)pc);
  switch (bc_op(cur)) {
  case BC_JFORL:
    if (bc_d(cur) != traceno)
      break;
    lj_assertJ(traceref_safe(J, bc_d(cur)) == T,
	       "JFORL references other trace");
    {
      BCIns *foripc = pc + bc_j(startins);
      lj_assertJ(bc_op(*foripc) == BC_JFORI || bc_op(*foripc) == BC_FORI,
		 "FORL does not point to JFORI");
      /* Restore the branch target before exposing the original FORL. */
      bc_publish_op(foripc, BC_FORI);
      bc_publish(pc, startins);
    }
    break;
  case BC_JITERL:
  case BC_JLOOP:
    if (bc_d(cur) != traceno)
      break;
    lj_assertJ(op == BC_ITERL || op == BC_ITERN || op == BC_LOOP ||
	       bc_isret(op), "bad original bytecode %d", op);
    bc_publish(pc, startins);
    break;
  case BC_JFUNCF:
    if (bc_d(cur) != traceno)
      break;
    lj_assertJ(op == BC_FUNCF, "bad original bytecode %d", op);
    bc_publish(pc, startins);
    break;
  default:  /* Already unpatched. */
    break;
  }
}

/* Flush a root trace. Returns 1 iff trace-exit publication changed. */
static uint32_t trace_flushroot(jit_State *J, GCtrace *T, int scoped)
{
  GCproto *pt = trace_startpt_acq(T);
  TraceNo traceno = trace_traceno_acq(T);
  TraceNo head;
  TraceNo nextroot = trace_nextroot_acq(T);
  uint32_t retargeted = 1;
  lj_assertJ(trace_root_acq(T) == 0, "not a root trace");
  lj_assertJ(pt != NULL, "trace has no prototype");
  if (LJ_UNLIKELY(pt == NULL))
    return 0;
  if (scoped) {
    uint32_t marked = trace_scope_mark_pending(T);
    /* A scoped root flush keeps the trace body and trace slot alive until the
    ** safepoint boundary, but no new interpreter dispatch should enter the
    ** pending body. The caller holds the JIT token until the boundary, so
    ** restoring the original bytecode here cannot race a replacement recorder;
    ** peers that already fetched JLOOP still take the pending-trace fallback.
    */
    if (marked)
      trace_unpatch(J, T);
    return marked;
  }
  head = proto_trace_acq(pt);
  trace_exittab_resetroot(J, traceno);
  /* Unlink root trace from chain anchored in prototype. */
  if (head == traceno) {  /* Trace is first in chain. Easy. */
    proto_trace_rel(pt, nextroot);
unpatch:
    /* Unpatch modified bytecode only if the trace has not been flushed. */
    trace_unpatch(J, T);
    return 1;
  } else if (head) {  /* Otherwise search in chain of root traces. */
    GCtrace *T2 = traceref_safe(J, head);
    if (T2) {
      TraceNo next;
      for (next = trace_nextroot_acq(T2); next;
	   next = T2 ? trace_nextroot_acq(T2) : 0) {
	if (next == traceno) {
	  trace_nextroot_rel(T2, nextroot);  /* Unlink from chain. */
	  goto unpatch;
	}
	T2 = traceref_safe(J, next);
      }
    }
  }
  /* Another scoped flush may have unlinked this root first. The guarded
  ** unpatch above is idempotent and only rewrites bytecode still naming T.
  */
  trace_unpatch(J, T);
  return retargeted;
}

static int trace_scope_flushing(jit_State *J, TraceNo traceno)
{
  global_State *g = J2G(J);
  int flushing = 0;
  if (traceno > 0 && traceno < trace_sizetrace_acq(J)) {
    GCtrace *T;
    lj_gc2_smr_read_enter(g);
    T = traceref_safe(J, traceno);
    flushing = T && trace_traceno_acq(T) == traceno &&
	       trace_scope_pending_acq(T);
    lj_gc2_smr_read_leave(g);
  }
  return flushing;
}

static uint32_t trace_flushside(jit_State *J, GCtrace *T, int scoped)
{
  IRIns base = ir_load_acq(&trace_ir_acq(T)[REF_BASE]);
  TraceNo parentno = (TraceNo)base.op1;
  GCtrace *parent = traceref_safe(J, parentno);
  ExitNo exitno = (ExitNo)base.op2;
  lj_assertJ(trace_root_acq(T) != 0, "not a side trace");
  if (scoped) {
    uint32_t marked = trace_scope_mark_pending(T);
    /* Once a scoped side trace is pending, no parent exit may enter it as a
    ** runnable body. The body and slot are still retired only after the
    ** safepoint boundary, but the inbound exit table edge must be disconnected
    ** at mark time to close the pending-but-enterable window.
    */
    if (marked && parent && trace_traceno_acq(parent) == parentno &&
	trace_exittab_acq(parent) && exitno < trace_nsnap_acq(parent))
      trace_exittarget_rel(parent, exitno, exitstub_addr(J, exitno));
    return marked;
  }
  trace_exittab_reset(J, T);
  if (parent && trace_traceno_acq(parent) == parentno &&
      trace_exittab_acq(parent) && exitno < trace_nsnap_acq(parent))
    trace_exittarget_rel(parent, exitno, exitstub_addr(J, exitno));
  return 1;
}

static void trace_unlink_side_chain(jit_State *J, GCtrace *T,
				    TraceNo traceno, TraceNo rootno)
{
  GCtrace *root = traceref_safe(J, rootno);
  if (root && trace_traceno_acq(root) == rootno) {
    TraceNo next = trace_nextside_acq(T);
    TraceNo head = trace_nextside_acq(root);
    if (head == traceno) {
      trace_nextside_rel(root, next);
      trace_nchild_dec_acqrel(root);
    } else if (head != 0) {
      GCtrace *prev = traceref_safe(J, head);
      while (prev) {
	TraceNo prevnext = trace_nextside_acq(prev);
	if (prevnext == traceno) {
	  trace_nextside_rel(prev, next);
	  trace_nchild_dec_acqrel(root);
	  break;
	}
	prev = prevnext ? traceref_safe(J, prevnext) : NULL;
      }
    }
  }
}

static void trace_retire_disconnect(jit_State *J, GCtrace *T)
{
  TraceNo traceno = trace_traceno_acq(T);
  TraceNo rootno = trace_root_acq(T);
  lj_assertJ(lj_jit_token_held(J),
	     "trace entry disconnection without recorder-token ownership");
  if (traceno == 0)
    return;
  /* retire_epoch is already non-zero here. Interpreter entry therefore fails
  ** closed while the token owner removes persistent bytecode and direct-mcode
  ** edges. The later epoch margin only has to cover readers which fetched an
  ** edge before this transaction; it is not asked to age out a permanent
  ** parent exit-table pointer.
  */
  lj_assertJ(la_load64_acq(&T->retire_epoch) != 0,
	     "trace edges disconnected before retirement publication");
  if (rootno == 0) {
    (void)trace_flushroot(J, T, 0);
  } else {
    (void)trace_flushside(J, T, 0);
    trace_unlink_side_chain(J, T, traceno, rootno);
  }
  trace_link_rel(T, 0);
  trace_nextroot_rel(T, 0);
  trace_nextside_rel(T, 0);
}

static int trace_scope_flush_dependency(jit_State *J, GCtrace *T)
{
  TraceNo link = trace_link_acq(T);
  TraceNo root = trace_root_acq(T);
  if (trace_scope_flushing(J, link))
    return 1;
  if (root != 0) {
    IRIns base = ir_load_acq(&trace_ir_acq(T)[REF_BASE]);
    TraceNo parent = (TraceNo)base.op1;
    if (trace_scope_flushing(J, root) ||
	trace_scope_flushing(J, parent))
      return 1;
  }
  return 0;
}

static uint32_t trace_flushscope_mark_deps(jit_State *J)
{
  global_State *g = J2G(J);
  uint32_t marked = 0, changed;
  do {
    TraceNo i;
    MSize sizetrace = trace_sizetrace_acq(J);
    changed = 0;
    lj_gc2_smr_read_enter(g);
    for (i = 1; i < sizetrace; i++) {
      GCtrace *T = traceref_safe(J, i);
      if (T && trace_traceno_acq(T) == i &&
			  !trace_scope_pending_acq(T) &&
			  trace_scope_flush_dependency(J, T)) {
	if (trace_root_acq(T) == 0) {
	  if (!trace_flushroot(J, T, 1))
	    continue;
	} else {
	  (void)trace_flushside(J, T, 1);
	}
	marked++;
	changed = 1;
      }
    }
    lj_gc2_smr_read_leave(g);
  } while (changed);
  return marked;
}

/* Flush a root or side trace. Returns non-zero iff scoped work was marked. */
uint32_t lj_trace_flush(jit_State *J, TraceNo traceno)
{
  global_State *g = J2G(J);
  uint32_t flushed = 0;
  if (traceno > 0 && traceno < trace_sizetrace_acq(J)) {
    GCtrace *T;
    lj_gc2_smr_read_enter(g);
    T = traceref_safe(J, traceno);
    if (T && trace_traceno_acq(T) == traceno) {
      if (trace_root_acq(T) == 0)
	flushed = trace_flushroot(J, T, 1);
      else
	flushed = trace_flushside(J, T, 1);
    }
    lj_gc2_smr_read_leave(g);
  }
  return flushed;
}

/* Unlink a trace without retiring its slot. Recorder aborts need this for the
** stock recursive-call path: trace_abort() must still see the return trace and
** self-link it as a blacklist entry.
*/
uint32_t lj_trace_flush_unlink(jit_State *J, TraceNo traceno)
{
  global_State *g = J2G(J);
  uint32_t unlinked = 0;
  if (traceno > 0 && traceno < trace_sizetrace_acq(J)) {
    GCtrace *T;
    lj_gc2_smr_read_enter(g);
    T = traceref_safe(J, traceno);
    if (T && trace_traceno_acq(T) == traceno) {
      trace_test_note_flush_unlink(T, traceno);
      if (trace_root_acq(T) == 0)
	unlinked = trace_flushroot(J, T, 0);
      else
	unlinked = trace_flushside(J, T, 0);
    }
    lj_gc2_smr_read_leave(g);
  }
  return unlinked;
}

static int trace_stale_startins_valid(GCproto *pt, const BCIns *pc,
				      BCIns startins)
{
  BCOp op = bc_op(startins);
  const BCIns *bc;
  if (!pt)
    return 1;
  bc = proto_bc(pt);
  if (pc < bc || pc >= bc + pt->sizebc)
    return 0;
  if (op == BC_FORL || op == BC_ITERL) {
    const BCIns *target = pc + 1 + ((int32_t)bc_d(startins) - BCBIAS_J);
    if (target < bc || target >= bc + pt->sizebc)
      return 0;
  }
  return 1;
}

static BCIns trace_stale_startins_match(GCtrace *T, const BCIns *pc,
					GCproto *owner)
{
  if (T && trace_startpc_acq(T) == pc) {
    BCIns startins = trace_startins_acq(T);
    BCOp op = bc_op(startins);
    if (op == BC_FORL || op == BC_ITERL || op == BC_LOOP ||
	op == BC_FUNCF || op == BC_ITERN || bc_isret(op)) {
      if (trace_stale_startins_valid(owner, pc, startins))
	return startins;
    }
  }
  return 0;
}

static BCIns trace_stale_startins_match_valid(global_State *g, GCtrace *T,
					      const BCIns *pc, GCproto *owner)
{
  if (!T || LJ_UNLIKELY(!trace_body_refs_valid(g, T, NULL)))
    return 0;
  return trace_stale_startins_match(T, pc, owner);
}

static GCtrace *trace_stale_startins_root_candidate(global_State *g, GCobj *o)
{
  GCtrace *T;
  uint32_t gct;
  if (!trace_preserve_body_candidate(g, o, &gct) ||
      gct != (uint32_t)~LJ_TTRACE)
    return NULL;
  T = gco2trace(o);
  if (LJ_UNLIKELY(!trace_body_refs_valid(g, T, NULL)))
    return NULL;
  return T;
}

#ifdef LJ_TRACE_TEST_HELPERS
int lj_trace_test_stale_startins_candidate(global_State *g, GCobj *o)
{
  return trace_stale_startins_root_candidate(g, o) != NULL;
}
#endif

static BCIns trace_stale_startins_root(global_State *g, const BCIns *pc,
				       GCproto *owner)
{
  GCobj *o;
  uint32_t n = 0;
  (void)lj_gc_flush_root_pending(g);
  for (o = lj_gc_root_acq(g); o != NULL;) {
    GCobj *next;
    if (!lj_gc2_obj_valid(g, o))
      break;
    next = lj_obj_gcw_acq(o);
    GCtrace *T = trace_stale_startins_root_candidate(g, o);
    if (T) {
      BCIns startins = trace_stale_startins_match(T, pc, owner);
      if (startins != 0)
	return startins;
    }
    if (++n >= 1000000u)
      break;
    o = next;
  }
  return 0;
}

BCIns LJ_FASTCALL lj_trace_stale_startins(jit_State *J, const BCIns *pc,
					  TraceNo traceno, lua_State *L)
{
  global_State *g = J2G(J);
  BCIns startins = 0;
  GCproto *owner = L && curr_funcisL(L) ? curr_proto(L) : NULL;
  lj_gc2_smr_read_enter(g);
  if (traceno > 0 && traceno < trace_sizetrace_acq(J))
    startins = trace_stale_startins_match_valid(g, traceref_safe(J, traceno),
						pc, owner);
  if (startins == 0) {
    TraceNo i, sizetrace = trace_sizetrace_acq(J);
    for (i = 1; i < sizetrace; i++) {
      startins = trace_stale_startins_match_valid(g, traceref_safe(J, i), pc,
						  owner);
      if (startins != 0)
	break;
    }
  }
  if (startins == 0) {
    GCtrace *T;
    for (T = trace_retired_head_acq(J);
	 T != NULL && lj_gc2_mem_registered(g, T);
	 T = trace_retired_next_acq(T)) {
      startins = trace_stale_startins_match_valid(g, T, pc, owner);
      if (startins != 0)
	break;
    }
  }
  if (startins == 0)
    startins = trace_stale_startins_root(g, pc, owner);
  lj_gc2_smr_read_leave(g);
  return startins;
}

/* Flush all traces associated with a prototype. */
uint32_t lj_trace_flushproto(global_State *g, GCproto *pt)
{
  jit_State *J = G2J(g);
  TraceNo trace;
  uint32_t flushed = 0;
  lj_gc2_smr_read_enter(g);
  for (trace = proto_trace_acq(pt); trace != 0; ) {
    GCtrace *T = traceref_safe(J, trace);
    if (!T || trace_traceno_acq(T) != trace)
      break;
    trace = trace_nextroot_acq(T);
    if (!trace_flushroot(J, T, 1))
      break;
    flushed++;
  }
  lj_gc2_smr_read_leave(g);
  return flushed;
}

static void trace_scope_clear_slot(jit_State *J, TraceNo traceno, GCtrace *T,
				   uint64_t epoch)
{
  global_State *g = J2G(J);
  lj_assertJ(lj_jit_token_held(J),
	     "scoped trace retirement without recorder-token ownership");
  /* Gate new entry and publish GC2 preservation before unlinking the root
  ** graph or inbound side-trace edge below.
  */
  trace_retire_at_epoch(g, T, epoch);
  trace_retire_disconnect(J, T);
  (void)lj_gdbjit_deltrace(J, T);
  if (trace_root_acq(T) == 0)
    trace_unpatch(J, T);
  /* Keep the trace number reserved until the retired body is reclaimable. */
  trace_slot_retire(J, T, traceno);
}

uint32_t lj_trace_flushscope_retire_hs(global_State *g, uint64_t epoch)
{
  jit_State *J = G2J(g);
  TraceNo i;
  uint32_t retired = 0;
  MSize sizetrace = trace_sizetrace_acq(J);
  /* This is the post-boundary token-owner half of scoped retirement. Generic
  ** EXIT_TRACES leaders only quiesce native users and abort recording: they may
  ** be unrelated GC workers and must never mutate trace slots on behalf of the
  ** parked token owner. Pending traces remain entry-gated until this call. */
  lj_assertJ(lj_jit_token_held(J),
	     "scoped retirement helper called without recorder token");
  if (!lj_jit_token_held(J))
    return 0;
  lj_gc2_smr_read_enter(g);
  for (i = 1; i < sizetrace; i++) {
    GCtrace *T = traceref_safe(J, i);
    if (T && trace_root_acq(T) != 0 && trace_traceno_acq(T) == i) {
      TraceNo rootno = trace_root_acq(T);
      GCtrace *root = traceref_safe(J, rootno);
      if (trace_scope_pending_acq(T) ||
	  (root && trace_traceno_acq(root) == rootno &&
	   trace_scope_pending_acq(root))) {
	trace_scope_clear_slot(J, i, T, epoch);
	retired++;
      }
    }
  }
  for (i = 1; i < sizetrace; i++) {
    GCtrace *T = traceref_safe(J, i);
    if (T && trace_root_acq(T) == 0 && trace_traceno_acq(T) == i &&
	trace_scope_pending_acq(T)) {
      trace_scope_clear_slot(J, i, T, epoch);
      retired++;
    }
  }
  lj_gc2_smr_read_leave(g);
  if (retired)
    gc2_jit_scoped_slots_retired_add(g, retired);
  return retired;
}

/* Flush all traces. */
int lj_trace_hasany(global_State *g)
{
  jit_State *J = G2J(g);
  MSize i, sizetrace = trace_sizetrace_acq(J);
  if (lj_trace_state_load(J) != LJ_TRACE_IDLE)
    return 1;  /* Active recorder must still be aborted by the boundary. */
  lj_gc2_smr_read_enter(g);
  for (i = 1; i < sizetrace; i++) {
    GCtrace *T = traceref_safe(J, i);
    if (T && trace_traceno_acq(T) == i) {
      lj_gc2_smr_read_leave(g);
      return 1;
    }
  }
  lj_gc2_smr_read_leave(g);
  return 0;
}

static int trace_flushall_direct(lua_State *L, int allow_gc_hook,
				 int send_event)
{
  jit_State *J = L2J(L);
  ptrdiff_t i;
  int token;
  if (!allow_gc_hook && (hookmask_load(J2G(J)) & HOOK_GC))
    return 1;
  token = lj_jit_token_acquire_wait(J);
  /*
  ** Full flush owns the recorder token on behalf of L. Publish that owner for
  ** mcode/native helpers used by the trace retirement pass.
  */
  jit_owner_l_rel(J, L);
  lj_gc2_smr_read_enter(J2G(J));
  for (i = (ptrdiff_t)trace_sizetrace_acq(J)-1; i > 0; i--) {
    GCtrace *T = traceref_safe(J, i);
    if (T && trace_traceno_acq(T) == (TraceNo)i) {
      /* The body must be on the preserved retire list before any prototype,
      ** side, bytecode, or trace-vector edge is removed.
      */
      trace_retire(J2G(J), T);
      trace_exittab_reset(J, T);
      if (trace_root_acq(T) == 0) {
	trace_flushroot(J, T, 0);
	trace_unpatch(J, T);
      }
      (void)lj_gdbjit_deltrace(J, T);
      /* Keep the trace number reserved until the retired body is reclaimable. */
      trace_link_rel(T, 0);
      trace_nextroot_rel(T, 0);
      trace_nextside_rel(T, 0);
      trace_slot_retire(J, T, (TraceNo)i);
      /*
      ** A peer may have fetched a patched JFORL/JITERL before this flush
      ** unpatched bytecode. Keep the body reachable through the retired list
      ** so the interpreter fallback can recover the original loop offset.
      */
    }
  }
  lj_gc2_smr_read_leave(J2G(J));
  J->cur.traceno = 0;
  J->freetrace = 0;
  /* Clear penalty cache. */
  memset(J->penalty, 0, sizeof(J->penalty));
  /* Free the whole machine code and invalidate all exit stub groups. */
  lj_mcode_free(J);
  memset(J->exitstubgroup, 0, sizeof(J->exitstubgroup));
  if (token)
    lj_jit_token_release(J);
  if (send_event) {
    lj_vmevent_send_l(L, TRACE,
      setstrV(V, V->top++, lj_str_newlit(V, "flush"));
    );
  }
  return 0;
}

int lj_trace_flushall(lua_State *L)
{
  return trace_flushall_direct(L, 0, 1);
}

int lj_trace_flushall_gc(lua_State *L)
{
  return trace_flushall_direct(L, 1, 0);
}

static TValue *trace_flush_vmevent_cp(lua_State *L, lua_CFunction dummy,
				      void *ud)
{
  global_State *g = G(L);
  UNUSED(dummy); UNUSED(ud);
#ifndef LUAJIT_DISABLE_VMEVENT
  if (vmevmask_load_acq(g) & VMEVENT_MASK(LJ_VMEVENT_TRACE)) {
    ptrdiff_t oldtop = savestack(L, L->top);
    ptrdiff_t argbase = lj_vmevent_prepare(L, LJ_VMEVENT_TRACE);
    if (argbase) {
      setstrV(L, L->top++, lj_str_newlit(L, "flush"));
      lj_vmevent_call(L, argbase, oldtop);
    }
  }
#else
  UNUSED(g);
#endif
  return NULL;
}

/* Request a leader-owned full trace flush through the safepoint protocol.
** Internal policy transitions use the eventless form: invoking a user TRACE
** callback while their lifecycle state is transitional permits a same-thread
** reentrant operation to self-wait forever. Public flushes retain the event. */
static int trace_flushall_hs_impl(lua_State *L, int send_event)
{
  global_State *g = G(L);
  jit_State *J = L2J(L);
  int errcode;
  int token;
  if ((hookmask_load(g) & HOOK_GC))
    return 1;
  if (gc2_n_threads_acq(g) <= 1 && mt_active_acq(g) == 0) {
    /* Before the first MT generation there is no remote trace user to quiesce.
    ** Use the direct path so the stock single-mutator jit.flush() keeps its TRACE
    ** "flush" vmevent. Once mt_active is sticky, even a one-TG gap must advance
    ** the safepoint epoch: otherwise every flush reserves another trace number at
    ** the same generation and the namespace can never reach its reuse grace.
    */
    return trace_flushall_direct(L, 0, send_event);
  }
  token = lj_jit_token_acquire_wait(J);
  (void)lj_gc2_handshake(g, LJ_GC2_HS_EXIT_TRACES|LJ_GC2_HS_FLUSHJ);
  /*
  ** The arbitrary safepoint leader uses the eventless GC flush path, but this
  ** caller owns L again after the handshake. Deliver the public TRACE "flush"
  ** event on that state while retaining the recorder token. The shared vmthread
  ** event path can race a peer TEXIT callback, and releasing the token first
  ** lets a new recorder have J->L overwritten by either event.
  */
  errcode = 0;
  if (send_event) {
    jit_owner_l_rel(J, L);
    errcode = lj_vm_cpcall(L, NULL, NULL, trace_flush_vmevent_cp);
  }
  if (token)
    lj_jit_token_release(J);
  if (errcode)
    lj_err_throw(L, errcode);  /* Propagate only after releasing the token. */
  return 0;
}

int lj_trace_flushall_hs(lua_State *L)
{
  return trace_flushall_hs_impl(L, 1);
}

int lj_trace_flushall_hs_noevent(lua_State *L)
{
  return trace_flushall_hs_impl(L, 0);
}

void lj_trace_flushscope_hs(global_State *g, uint32_t work)
{
  if (work != 0) {
    jit_State *J = G2J(g);
    int token = lj_jit_token_acquire_wait(J);
    (void)trace_flushscope_mark_deps(G2J(g));
    (void)lj_gc2_handshake(g, LJ_GC2_HS_EXIT_TRACES);  /* 08 section 8.7 scoped boundary. */
    (void)lj_trace_flushscope_retire_hs(g, lj_gc2_retire_epoch(g));
    if (token)
      lj_jit_token_release(J);
  }
}

uint32_t lj_trace_flushscope(jit_State *J, TraceNo traceno)
{
  int token = lj_jit_token_acquire_wait(J);
  uint32_t work = lj_trace_flush(J, traceno);
  lj_trace_flushscope_hs(J2G(J), work);
  if (token)
    lj_jit_token_release(J);
  return work;
}

/* Initialize JIT compiler state. */
void lj_trace_initstate(global_State *g)
{
  jit_State *J = G2J(g);
  TValue *tv;

  /* Initialize aligned SIMD constants. */
  tv = LJ_KSIMD(J, LJ_KSIMD_ABS);
  tv[0].u64 = U64x(7fffffff,ffffffff);
  tv[1].u64 = U64x(7fffffff,ffffffff);
  tv = LJ_KSIMD(J, LJ_KSIMD_NEG);
  tv[0].u64 = U64x(80000000,00000000);
  tv[1].u64 = U64x(80000000,00000000);

  /* Initialize 32/64 bit constants. */
  J->k64[LJ_K64_M2P64].u64 = U64x(c3f00000,00000000);
#if LJ_TARGET_X86ORX64
  J->k64[LJ_K64_TOBIT].u64 = U64x(43380000,00000000);
  J->k64[LJ_K64_2P64].u64 = U64x(43f00000,00000000);
#endif
#if LJ_TARGET_MIPS64
  J->k64[LJ_K64_2P63].u64 = U64x(43e00000,00000000);
#endif
#if LJ_TARGET_MIPS
  J->k64[LJ_K64_2P31].u64 = U64x(41e00000,00000000);
#endif

#if LJ_TARGET_X86ORX64 || LJ_TARGET_MIPS64
  J->k32[LJ_K32_M2P64] = 0xdf800000;
#endif
#if LJ_TARGET_MIPS64
  J->k32[LJ_K32_2P63] = 0x5f000000;
#endif
#if LJ_TARGET_PPC
  J->k32[LJ_K32_2P52_2P31] = 0x59800004;
  J->k32[LJ_K32_2P52] = 0x59800000;
#endif
#if LJ_TARGET_PPC
  J->k32[LJ_K32_2P31] = 0x4f000000;
#endif

#if LJ_TARGET_PPC || LJ_TARGET_MIPS32
  J->k32[LJ_K32_VM_EXIT_HANDLER] = (uintptr_t)(void *)lj_vm_exit_handler;
  J->k32[LJ_K32_VM_EXIT_INTERP] = (uintptr_t)(void *)lj_vm_exit_interp;
#endif
#if LJ_TARGET_ARM64 || LJ_TARGET_MIPS64
  J->k64[LJ_K64_VM_EXIT_HANDLER].u64 = (uintptr_t)lj_ptr_sign((void *)lj_vm_exit_handler, 0);
  J->k64[LJ_K64_VM_EXIT_INTERP].u64 = (uintptr_t)lj_ptr_sign((void *)lj_vm_exit_interp, 0);
#endif
}

/* Free everything associated with the JIT compiler state. */
void lj_trace_freestate(global_State *g)
{
  jit_State *J = G2J(g);
#ifdef LUA_USE_ASSERT
  {  /* Live slots are gone. Reserved retired slots may still name bodies until
      ** the active vector is freed below and the retire list drains. */
    ptrdiff_t i;
    ptrdiff_t sizetrace = (ptrdiff_t)trace_sizetrace_acq(J);
    for (i = 1; i < sizetrace; i++) {
      GCtrace *T = traceref_safe(J, (TraceNo)i);
      lj_assertG(i == (ptrdiff_t)J->cur.traceno || T == NULL ||
		 (trace_traceno_acq(T) == 0 &&
		  la_load64_acq(&T->retire_epoch) != 0 &&
		  trace_retired_link_listed_acq(T)),
		 "live trace remained at VM close");
    }
  }
#endif
  lj_mem_freevec(g, J->snapmapbuf, J->sizesnapmap, SnapEntry);
  lj_mem_freevec(g, J->snapbuf, J->sizesnap, SnapShot);
  lj_mem_freevec(g, J->irbuf + J->irbotlim, J->irtoplim - J->irbotlim, IRIns);
  {
    TraceVec *tv = tracevec_acq(J);
    if (tv) {
      trace_sizetrace_rel(J, 0);
      tracevec_rel(J, NULL);
      tracevec_free(g, tv);
    }
  }
  lj_trace_freeretired(g);
  lj_mcode_freeall(g);
}

/* -- Penalties and blacklisting ------------------------------------------ */

/* Blacklist a bytecode instruction. */
static void blacklist_pc(GCproto *pt, BCIns *pc)
{
  if (bc_op(*pc) == BC_ITERN) {
    bc_publish_op(pc, BC_ITERC);
    bc_publish_op(pc+1+bc_j(pc[1]), BC_JMP);
  } else {
    bc_publish_op(pc, (int)bc_op(*pc)+(int)BC_ILOOP-(int)BC_LOOP);
    pt->flags |= PROTO_ILOOP;
  }
}

/* Penalize a bytecode instruction. */
static void penalty_pc(jit_State *J, GCproto *pt, BCIns *pc, TraceError e)
{
  uint32_t i, val = PENALTY_MIN;
  for (i = 0; i < PENALTY_SLOTS; i++)
    if (mref(J->penalty[i].pc, const BCIns) == pc) {  /* Cache slot found? */
      /* First try to bump its hotcount several times. */
      val = ((uint32_t)J->penalty[i].val << 1) +
	    (lj_prng_u64(&J2TG(J)->prng) & ((1u<<PENALTY_RNDBITS)-1));
      if (val > PENALTY_MAX) {
	blacklist_pc(pt, pc);  /* Blacklist it, if that didn't help. */
	return;
      }
      goto setpenalty;
    }
  /* Assign a new penalty cache slot. */
  i = J->penaltyslot;
  J->penaltyslot = (J->penaltyslot + 1) & (PENALTY_SLOTS-1);
  setmref(J->penalty[i].pc, pc);
setpenalty:
  J->penalty[i].val = (uint16_t)val;
  J->penalty[i].reason = e;
  hotcount_setg(J2G(J), pc+1, val);
}

static void trace_mark_active_startpt(jit_State *J)
{
  global_State *g;
  GCobj *o;
  if (!J || !J->L || !J->pt || !checkptrGC(J->pt) ||
      J->pt->gct != ~LJ_TPROTO)
    return;
  g = J2G(J);
  o = obj2gco(J->pt);
  /*
  ** The recorder starts with only J->pt; J->cur.startpt is published later after
  ** trace-number allocation. The start prototype is already a semantic root in
  ** that setup window, and remains one until recording publishes or aborts.
  */
  if (gc2_phase_acq(g) != LJ_GC2_IDLE)
    (void)lj_gc2_trace_sweep_root(g, o);
  lj_gc2_barrier_marked_proto(J->L, J->pt);
}

/* -- Trace compiler state machine ---------------------------------------- */

/* Start tracing. */
static void trace_start(jit_State *J)
{
  TraceNo traceno;
  uint32_t gc2phase = gc2_phase_acq(J2G(J));

  /* MARK/WEAK still need a complete recorder-root barrier proof. SWEEP has
  ** already frozen and boundedly detached the old ownership generation; new
  ** trace/root/sidecar allocations are post-reset objects, while
  ** trace_mark_active_startpt() explicitly preserves the prototype through the
  ** sweep bridge. Refusing SWEEP here can starve recording forever in an
  ** allocation-free hot loop, because no later allocation would drive the
  ** bounded quarantine owner back to IDLE.
  */
  if (gc2phase != LJ_GC2_IDLE && gc2phase != LJ_GC2_SWEEP) {
    /* This is a transient collector gate, not a recorder penalty. Leaving the
    ** ordinary full hotloop reset in place can consume every hot event while a
    ** peer-owned MARK/WEAK cycle is active, so a finite loop never records even
    ** after GC2 returns to IDLE. Keep a root edge on a short retry backoff; the
    ** token acquisition remains a bounded try and trace_start() still refuses
    ** to publish anything until the phase is safe. */
    if (J->parent == 0 && J->pc != NULL)
      hotcount_setg(J2G(J), J->pc+1, 16);
    lj_trace_state_store(J, LJ_TRACE_IDLE);
    return;
  }
  trace_mark_active_startpt(J);
  if ((J->pt->flags & PROTO_NOJIT)) {  /* JIT disabled for this proto? */
    if (J->parent == 0 && J->exitno == 0 && bc_op(*J->pc) != BC_ITERN) {
      /* Lazy bytecode patching to disable hotcount events. */
      lj_assertJ(bc_op(*J->pc) == BC_FORL || bc_op(*J->pc) == BC_ITERL ||
		 bc_op(*J->pc) == BC_LOOP || bc_op(*J->pc) == BC_FUNCF,
		 "bad hot bytecode %d", bc_op(*J->pc));
      bc_publish_op(J->pc, (int)bc_op(*J->pc)+(int)BC_ILOOP-(int)BC_LOOP);
      J->pt->flags |= PROTO_ILOOP;
    }
    lj_trace_state_store(J, LJ_TRACE_IDLE);  /* Silently ignored. */
    return;
  }

  /* Ensuring forward progress for BC_ITERN can trigger hotcount again. */
  if (!J->parent && bc_op(*J->pc) == BC_JLOOP) {  /* Already compiled. */
    lj_trace_state_store(J, LJ_TRACE_IDLE);  /* Silently ignored. */
    return;
  }

  /* Get a new trace number. */
  traceno = trace_findfree(J);
  if (LJ_UNLIKELY(traceno == 0)) {  /* No free trace? */
    lj_assertJ((hookmask_load(J2G(J)) & HOOK_GC) == 0,
	       "recorder called from GC hook");
    (void)lj_trace_flushall_hs(J->L);
    lj_trace_state_store(J, LJ_TRACE_IDLE);  /* Silently ignored. */
    return;
  }
  traceslot_pending(J, traceno);

  /* Setup enough of the current trace to be able to send the vmevent. */
  memset(&J->cur, 0, sizeof(GCtrace));
  J->cur.traceno = traceno;
  J->cur.nins = J->cur.nk = REF_BASE;
  J->cur.ir = J->irbuf;
  J->cur.snap = J->snapbuf;
  J->cur.snapmap = J->snapmapbuf;
  J->mergesnap = 0;
  J->needsnap = 0;
  J->bcskip = 0;
  J->guardemit.irt = 0;
  J->postproc = LJ_POST_NONE;
  lj_resetsplit(J);
  J->retryrec = 0;
  J->ktrace = 0;
  trace_startpt_rel(&J->cur, J->pt);
  trace_mark_active_startpt(J);

  lj_vmevent_send_l_(J->L, TRACE,
    TValue savetv = J2TG(J)->tmptv;
    TValue savetv2 = J2TG(J)->tmptv2;
    TraceNo parent = J->parent;
    ExitNo exitno = J->exitno;
    setstrV(V, V->top++, lj_str_newlit(V, "start"));
    setintV(V->top++, traceno);
    setfuncV(V, V->top++, J->fn);
    setintV(V->top++, proto_bcpos(J->pt, J->pc));
    if (J->parent) {
      setintV(V->top++, J->parent);
      setintV(V->top++, J->exitno);
    } else {
      BCOp op = bc_op(*J->pc);
      if (op == BC_CALLM || op == BC_CALL || op == BC_ITERC) {
	setintV(V->top++, J->exitno);  /* Parent of stitched trace. */
	setintV(V->top++, -1);
      }
    }
  ,
    J2TG(J)->tmptv = savetv;
    J2TG(J)->tmptv2 = savetv2;
    J->parent = parent;
    J->exitno = exitno;
  );
  lj_record_setup(J);
}

/* Stop tracing. */
static void trace_stop(jit_State *J)
{
  global_State *g = J2G(J);
  BCIns *pc = mref(J->cur.startpc, BCIns);
  BCOp op = bc_op(J->cur.startins);
  GCproto *pt = trace_startpt_acq(&J->cur);
  TraceNo traceno = J->cur.traceno;
  TraceNo parentno = 0, rootno = 0;
  ExitNo exitno = J->exitno;
  GCtrace *T = J->curfinal;
  BCIns *patchpc = NULL;
  BCIns patchins = 0;
  GCtrace *parent = NULL;
  GCtrace *root = NULL;
  SnapShot *snap = NULL;
  MSize topslot;
  int addroot = 0;

  switch (op) {
  case BC_FORL:
    /* The matching FORI is patched after trace publication. */
    /* fallthrough */
  case BC_LOOP:
  case BC_ITERL:
  case BC_FUNCF:
    patchpc = pc;
    patchins = BCINS_AD((int)op+(int)BC_JLOOP-(int)BC_LOOP,
			bc_a(J->cur.startins), traceno);
  addroot:
    J->cur.nextroot = (TraceNo1)proto_trace_acq(pt);
    addroot = 1;
    break;
  case BC_ITERN:
  case BC_RET:
  case BC_RET0:
  case BC_RET1:
    patchpc = pc;
    patchins = BCINS_AD(BC_JLOOP, J->cur.snap[0].nslots, traceno);
    goto addroot;
  case BC_JMP:
    parentno = J->parent;
    rootno = J->cur.root;
    lj_assertJ(parentno != 0 && rootno != 0, "not a side trace");
    lj_gc2_smr_read_enter(g);
    parent = traceref_safe(J, parentno);
    root = traceref_safe(J, rootno);
    lj_assertJ(parent != NULL && root != NULL, "missing parent/root trace");
    /* Avoid compiling a side trace twice (stack resizing uses parent exit). */
    J->cur.nextside = (TraceNo1)trace_nextside_acq(root);
    lj_gc2_smr_read_leave(g);
    break;
  case BC_CALLM:
  case BC_CALL:
  case BC_ITERC:
    parentno = (TraceNo)J->exitno;
    lj_gc2_smr_read_enter(g);
    parent = traceref_safe(J, parentno);
    lj_assertJ(parent != NULL, "missing stitched trace");
    lj_gc2_smr_read_leave(g);
    break;
  default:
    lj_assertJ(0, "bad stop bytecode %d", op);
    break;
  }

  /* Commit and publish the final trace before enabling bytecode/exits. */
  lj_mcode_commit(J, J->cur.mcode);
  lj_mcode_sync_core(J);
  J->postproc = LJ_POST_NONE;
  trace_save(J, T);

  switch (op) {
  case BC_FORL:
    /* Leave FORI unpatched. JFORL carries the trace number by itself; avoiding
    ** the paired JFORI/JFORL publication race preserves numeric-for semantics
    ** when another TG is currently executing the first-iteration opcode.
    */
    /* fallthrough */
  case BC_LOOP:
  case BC_ITERL:
  case BC_FUNCF:
  case BC_ITERN:
  case BC_RET:
  case BC_RET0:
  case BC_RET1:
    if (addroot)
      proto_trace_rel(pt, traceno);
    if (patchpc)
      bc_publish(patchpc, patchins);
    break;
  case BC_JMP:
    lj_gc2_smr_read_enter(g);
    parent = traceref_safe(J, parentno);
    root = traceref_safe(J, rootno);
    lj_assertJ(parent != NULL && root != NULL, "missing parent/root trace");
    lj_assertJ(trace_exittab_acq(parent) != NULL, "missing parent exit table");
    snap = &trace_snap_acq(parent)[exitno];
    topslot = trace_topslot_acq(T);
    if (topslot > snap_topslot_acq(snap)) snap_topslot_rel(snap, topslot);
    trace_nchild_inc_acqrel(root);
    trace_nextside_rel(root, traceno);
    snap_count_rel(snap, SNAPCOUNT_DONE);
    /*
    ** The parent exit target is the runnable side-trace gate. Publish it only
    ** after the parent/root metadata above can be observed by other threads.
    */
    trace_exittarget_rel(parent, exitno, trace_mcode_acq(T));
    lj_gc2_smr_read_leave(g);
    break;
  case BC_CALLM:
  case BC_CALL:
  case BC_ITERC:
    lj_gc2_smr_read_enter(g);
    parent = traceref_safe(J, parentno);
    lj_assertJ(parent != NULL, "missing stitched trace");
    trace_link_rel(parent, traceno);
    lj_gc2_smr_read_leave(g);
    break;
  default:
    break;
  }

  lj_vmevent_send_l(J->L, TRACE,
    setstrV(V, V->top++, lj_str_newlit(V, "stop"));
    setintV(V->top++, traceno);
    setfuncV(V, V->top++, J->fn);
  );
}

/* Start a new root trace for down-recursion. */
static int trace_downrec(jit_State *J)
{
  /* Restart recording at the return instruction. */
  lj_assertJ(J->pt != NULL, "no active prototype");
  lj_assertJ(bc_isret(bc_op(*J->pc)), "not at a return bytecode");
  if (bc_op(*J->pc) == BC_RETM)
    return 0;  /* NYI: down-recursion with RETM. */
  J->parent = 0;
  J->exitno = 0;
  if (lj_trace_state_aborted(lj_trace_state_store_active(J, LJ_TRACE_RECORD)))
    return 0;
  trace_start(J);
  return 1;
}

/* Abort tracing. */
static int trace_abort(jit_State *J)
{
  lua_State *L = J->L;
  TraceError e = LJ_TRERR_RECERR;
  TraceNo traceno;

  J->postproc = LJ_POST_NONE;
  lj_mcode_abort(J);
  if (J->curfinal) {
    lj_trace_free_unpublished(J2G(J), J->curfinal);
    J->curfinal = NULL;
  }
  if (tvisnumber(L->top-1))
    e = (TraceError)numberVint(L->top-1);
  /* MCODELM retries rebuild per-trace exit stubs in a fresh mcode area. */
  trace_exittab_free(J2G(J), &J->cur, J->cur.nsnap);
  if (e == LJ_TRERR_MCODELM) {
    L->top--;  /* Remove error object */
    if (lj_trace_state_aborted(lj_trace_state_store_active(J, LJ_TRACE_ASM)))
      return 0;
    return 1;  /* Retry ASM with new MCode area. */
  }
  /* Penalize or blacklist starting bytecode instruction. */
  if (J->parent == 0 && !bc_isret(bc_op(J->cur.startins))) {
    if (J->exitno == 0) {
      BCIns *startpc = mref(J->cur.startpc, BCIns);
      if (e == LJ_TRERR_RETRY)
	hotcount_setg(J2G(J), startpc+1, 1);  /* Immediate retry. */
      else
	penalty_pc(J, trace_startpt_acq(&J->cur), startpc, e);
    } else {
      TraceNo selflink = (TraceNo)J->exitno;
      GCtrace *T;
      lj_gc2_smr_read_enter(J2G(J));
      T = traceref_safe(J, selflink);
      if (T && trace_traceno_acq(T) == selflink) {
	trace_test_note_abort_selflink(selflink);
	trace_link_rel(T, selflink);  /* Self-link is blacklisted. */
      }
      lj_gc2_smr_read_leave(J2G(J));
    }
  }

  /* Is there anything to abort? */
  traceno = J->cur.traceno;
  if (traceno) {
    J->cur.link = 0;
    J->cur.linktype = LJ_TRLINK_NONE;
    lj_vmevent_send_l(L, TRACE,
      cTValue *bot = tvref(L->stack)+LJ_FR2;
      cTValue *frame;
      const BCIns *pc;
      BCPos pos = 0;
      setstrV(V, V->top++, lj_str_newlit(V, "abort"));
      setintV(V->top++, traceno);
      /* Find original Lua function call to generate a better error message. */
      for (frame = L->base-1, pc = J->pc; ; frame = frame_prev(frame)) {
	if (isluafunc(frame_func(frame))) {
	  pos = proto_bcpos(funcproto(frame_func(frame)), pc);
	  break;
	} else if (frame_prev(frame) <= bot) {
	  break;
	} else if (frame_iscont(frame)) {
	  pc = frame_contpc(frame) - 1;
	} else {
	  pc = frame_pc(frame) - 1;
	}
      }
      setfuncV(V, V->top++, frame_func(frame));
      setintV(V->top++, pos);
      copyTV(V, V->top++, restorestack(L, vmevtop)-1);
      copyTV(V, V->top++, &J->errinfo);
    );
    /* Drop aborted trace after the vmevent (which may still access it). */
    traceslot_clear(J, traceno);
    if (traceno < J->freetrace)
      J->freetrace = traceno;
    J->cur.traceno = 0;
  }
  L->top--;  /* Remove error object */
  if (e == LJ_TRERR_DOWNREC) {
    return trace_downrec(J);
  } else if (e == LJ_TRERR_MCODEAL) {
    if (!J->mcarea) {  /* Disable JIT compiler if first mcode alloc fails. */
      jit_flags_setmask(J, JIT_F_ON, 0);
      lj_dispatch_update(J2G(J), 0);
    }
    (void)lj_trace_flushall_hs(L);
  }
  return 0;
}

/* Perform pending re-patch of a bytecode instruction. */
static LJ_AINLINE void trace_pendpatch(jit_State *J, int force)
{
  if (LJ_UNLIKELY(J->patchpc)) {
    if (force || J->bcskip == 0) {
      BCIns patchins = J->patchins;
      BCOp op = bc_op(patchins);
      if (op == BC_JFORL || op == BC_JITERL || op == BC_JLOOP ||
	  op == BC_JFUNCF) {
	TraceNo traceno = bc_d(patchins);
	GCtrace *T;
	lj_gc2_smr_read_enter(J2G(J));
	T = traceref_safe(J, traceno);
	if (trace_runnable_acq(T, traceno))
	  bc_publish(J->patchpc, patchins);
	lj_gc2_smr_read_leave(J2G(J));
      } else {
	bc_publish(J->patchpc, patchins);
      }
      J->patchpc = NULL;
    } else {
      J->bcskip = 0;
    }
  }
}

/* State machine for the trace compiler. Protected callback. */
static TValue *trace_state(lua_State *L, lua_CFunction dummy, void *ud)
{
  jit_State *J = (jit_State *)ud;
  UNUSED(dummy);
  do {
  retry:
    switch ((uint32_t)lj_trace_state_load(J)) {
    case LJ_TRACE_START:
      if (lj_trace_state_aborted(
	    lj_trace_state_store_active(J, LJ_TRACE_RECORD)))
	goto retry;  /* trace_start() may change state. */
      trace_start(J);
      lj_dispatch_update(J2G(J), 0);
      if (lj_trace_state_aborted(lj_trace_state_load(J)))
	goto retry;
      if (lj_trace_state_load(J) != LJ_TRACE_RECORD_1ST)
	break;
      /* fallthrough */

    case LJ_TRACE_RECORD_1ST:
      if (lj_trace_state_aborted(
	    lj_trace_state_store_active(J, LJ_TRACE_RECORD)))
	goto retry;
      /* fallthrough */
    case LJ_TRACE_RECORD:
      trace_pendpatch(J, 0);
      setvmstate(J2G(J), RECORD);
      lj_vmevent_send_l_(J->L, RECORD,
	/* Save/restore state for trace recorder. */
	TValue savetv = J2TG(J)->tmptv;
	TValue savetv2 = J2TG(J)->tmptv2;
	TraceNo parent = J->parent;
	ExitNo exitno = J->exitno;
	setintV(V->top++, J->cur.traceno);
	setfuncV(V, V->top++, J->fn);
	setintV(V->top++, J->pt ? (int32_t)proto_bcpos(J->pt, J->pc) : -1);
	setintV(V->top++, J->framedepth);
      ,
	J2TG(J)->tmptv = savetv;
	J2TG(J)->tmptv2 = savetv2;
	J->parent = parent;
	J->exitno = exitno;
      );
      lj_record_ins(J);
      break;

    case LJ_TRACE_END:
      trace_pendpatch(J, 1);
      J->loopref = 0;
      if ((jit_flags_acq(J) & JIT_F_OPT_LOOP) &&
	  J->cur.link == J->cur.traceno && J->framedepth + J->retdepth == 0) {
	setvmstate(J2G(J), OPT);
	lj_opt_dce(J);
	if (lj_opt_loop(J)) {  /* Loop optimization failed? */
	  J->cur.link = 0;
	  J->cur.linktype = LJ_TRLINK_NONE;
	  J->loopref = J->cur.nins;
	  if (lj_trace_state_aborted(
		lj_trace_state_store_active(J, LJ_TRACE_RECORD)))
	    goto retry;  /* Try to continue recording. */
	  break;
	}
	J->loopref = J->chain[IR_LOOP];  /* Needed by assembler. */
      }
      lj_opt_split(J);
      lj_opt_sink(J);
      if (!J->loopref) J->cur.snap[J->cur.nsnap-1].count = SNAPCOUNT_DONE;
      if (lj_trace_state_aborted(
	    lj_trace_state_store_active(J, LJ_TRACE_ASM)))
	goto retry;
      break;

    case LJ_TRACE_ASM:
      setvmstate(J2G(J), ASM);
      lj_asm_trace(J, &J->cur);
      if (lj_trace_state_aborted(lj_trace_state_load(J)))
	goto retry;
      trace_stop(J);
      setvmstate(J2G(J), INTERP);
      lj_trace_state_store(J, LJ_TRACE_IDLE);
      lj_dispatch_update(J2G(J), 0);
      lj_jit_token_release_l(J->L, J);
      if (gc2_phase_acq(G(L)) != LJ_GC2_IDLE || lj_gc_should_step(G(L)))
	lj_gc_step(L);
      return NULL;

    default:  /* Trace aborted asynchronously. */
      setintV(L->top++, (int32_t)LJ_TRERR_RECERR);
      /* fallthrough */
    /* lj_err_throw() clears ACTIVE for synchronous recorder errors, too. */
    case (LJ_TRACE_ERR & ~LJ_TRACE_ACTIVE):
    case LJ_TRACE_ERR:
      trace_pendpatch(J, 1);
      if (trace_abort(J))
	goto retry;
      setvmstate(J2G(J), INTERP);
      lj_trace_state_store(J, LJ_TRACE_IDLE);
      lj_dispatch_update(J2G(J), 0);
      lj_jit_token_release_l(J->L, J);
      return NULL;
    }
  } while (lj_trace_state_load(J) > LJ_TRACE_RECORD);
  if (lj_trace_state_aborted(lj_trace_state_load(J)))
    goto retry;
  if (lj_trace_state_load(J) == LJ_TRACE_IDLE)
    lj_jit_token_release_l(J->L, J);
  return NULL;
}

/* Cancel unpublished recorder state before its owning TG detaches. */
void lj_trace_abort_owner(lua_State *L)
{
  jit_State *J;
  global_State *g;
  TraceNo traceno;
  if (!L)
    return;
  J = L2J(L);
  /* Prove ownership before reading or mutating any token-private recorder
  ** field. J->L may name a coroutine on this TG, not necessarily L itself.
  */
  if (!lj_jit_token_held_l(L, J))
    return;
  g = J2G(J);
  lj_trace_state_abort(J);
  trace_pendpatch(J, 1);
  J->postproc = LJ_POST_NONE;
  lj_mcode_abort(J);
  if (J->curfinal) {
    lj_trace_free_unpublished(g, J->curfinal);
    J->curfinal = NULL;
  }
  trace_exittab_free(g, &J->cur, J->cur.nsnap);
  traceno = J->cur.traceno;
  if (traceno) {
    J->cur.link = 0;
    J->cur.linktype = LJ_TRLINK_NONE;
    traceslot_clear(J, traceno);
    if (traceno < J->freetrace)
      J->freetrace = traceno;
  }
  memset(&J->cur, 0, sizeof(J->cur));
  J->patchpc = NULL;
  J->patchins = 0;
  J->bcskip = 0;
  J->mergesnap = 0;
  J->needsnap = 0;
  J->guardemit.irt = 0;
  J->retryrec = 0;
  J->loopref = 0;
  J->ktrace = 0;
  J->parent = 0;
  J->exitno = 0;
  /* Do not run trace_abort(): teardown occurs after lua_pcall has unwound the
  ** recorded frame, so penalty, down-recursion and TRACE-abort event paths must
  ** not inspect J->pc or walk L's now-different frame chain.
  */
  setvmstate(g, INTERP);
  lj_trace_state_store(J, LJ_TRACE_IDLE);
  lj_dispatch_update(g, 0);
  lj_jit_token_release_l(L, J);
}

/* -- Event handling ------------------------------------------------------ */

/* A bytecode instruction is about to be executed. Record it. */
void lj_trace_ins(jit_State *J, const BCIns *pc)
{
  /* Note: J->L must already be set. pc is the true bytecode PC here. */
  J->pc = pc;
  J->fn = curr_func(J->L);
  J->pt = isluafunc(J->fn) ? funcproto(J->fn) : NULL;
  while (lj_vm_cpcall(J->L, NULL, (void *)J, trace_state) != 0)
    lj_trace_state_store_active(J, LJ_TRACE_ERR);
}

static int trace_hot_root_start_valid(const BCIns *pc)
{
  /* Hotcount events are edge-triggered against mutable bytecode. Another
  ** thread may publish a JIT or disabled variant while this TG is reaching
  ** the recorder. Only unpatched root-start bytecodes may enter
  ** rec_setup_root(); patched bytecode should redispatch normally.
  */
  BCOp op = bc_op((BCIns)la_load32_acq((uint32_t *)pc));
  return op == BC_FORL || op == BC_ITERL || op == BC_ITERN ||
	 op == BC_LOOP || op == BC_FUNCF;
}

/* A hotcount triggered. Start recording a root trace. */
void LJ_FASTCALL lj_trace_hot(jit_State *J, const BCIns *pc, lua_State *L)
{
  /* Note: pc is the interpreter bytecode PC here. It's offset by 1. */
  ERRNO_SAVE
  /* Reset hotcount. */
  hotcount_setg(J2G(J), pc, jit_param_acq(J, JIT_P_hotloop)*HOTCOUNT_LOOP);
  /* Only start a new trace if not recording or inside __gc call or vmevent. */
  if (lj_trace_state_load(J) == LJ_TRACE_IDLE &&
      !(hookmask_load(J2G(J)) & (HOOK_GC|HOOK_VMEVENT)) &&
      lj_jit_token_try_l(L, J)) {
    jit_owner_l_rel(J, L);
    if (!trace_hot_root_start_valid(pc-1)) {
      lj_jit_token_release_l(L, J);
      ERRNO_RESTORE
      return;
    }
    J->parent = 0;  /* Root trace. */
    J->exitno = 0;
    if (!lj_trace_state_aborted(
		  lj_trace_state_store_active(J, LJ_TRACE_START)))
      lj_trace_ins(J, pc-1);
    else
      lj_jit_token_release_l(L, J);
  }
  ERRNO_RESTORE
}

/* Check for a hot side exit. If yes, start recording a side trace. */
static void trace_hotside(jit_State *J, const BCIns *pc, lua_State *L,
			  TraceNo parent, ExitNo exitno)
{
  global_State *g = J2G(J);
  GCtrace *parentT;
  SnapShot *snap;
  uint32_t hotexit = (uint32_t)jit_param_acq(J, JIT_P_hotexit);
  uint8_t count;
  lj_gc2_smr_read_enter(g);
  parentT = traceref_safe(J, parent);
  if (!trace_runnable_acq(parentT, parent) || exitno >= trace_nsnap_acq(parentT))
    goto out;
  {
    TraceNo rootno = trace_root_acq(parentT);
    GCtrace *root = rootno ? traceref_safe(J, rootno) : parentT;
    GCproto *pt = root ? trace_startpt_acq(root) : NULL;
    /*
    ** Root and first-level side traces have the complete original local-cell
    ** snapshot shape. Only an active-MT side-of-side would replay a snapshot
    ** that has already replayed CGET/CSET cells; leave that chain interpreted
    ** until generated code carries the nested-cell proof.
    */
    if (rootno != 0 && pt && proto_cellops(pt) &&
	lj_record_mt_runtime_shared(g, L))
      goto out;
  }
  snap = &trace_snap_acq(parentT)[exitno];
  if (!(hookmask_load(g) & (HOOK_GC|HOOK_VMEVENT)) &&
      isluafunc(curr_func(L))) {
    for (;;) {
      count = (uint8_t)snap_count_acq(snap);
      if (count == SNAPCOUNT_DONE)
	goto out;
      if ((uint32_t)count + 1u >= hotexit)
	break;
      if (snap_count_cas_acqrel(snap, &count, count + 1u))
	goto out;
    }
    if (lj_trace_state_load(J) != LJ_TRACE_IDLE)
      goto out;
    if (!lj_jit_token_try_l(L, J))
      goto out;
    parentT = traceref_safe(J, parent);
    if (!trace_runnable_acq(parentT, parent) ||
	exitno >= trace_nsnap_acq(parentT)) {
      lj_jit_token_release_l(L, J);
      goto out;
    }
    snap = &trace_snap_acq(parentT)[exitno];
    for (;;) {
      count = (uint8_t)snap_count_acq(snap);
      if (count == SNAPCOUNT_DONE) {
	lj_jit_token_release_l(L, J);
	goto out;
      }
      if (count >= SNAPCOUNT_DONE-1 ||
	  snap_count_cas_acqrel(snap, &count, count + 1u))
	break;
    }
    jit_owner_l_rel(J, L);
    J->parent = parent;
    J->exitno = exitno;
    /* J->parent is non-zero for a side trace. */
    lj_gc2_smr_read_leave(g);
    if (!lj_trace_state_aborted(
	      lj_trace_state_store_active(J, LJ_TRACE_START)))
      lj_trace_ins(J, pc);
    else
      lj_jit_token_release_l(L, J);
    return;
  }
out:
  lj_gc2_smr_read_leave(g);
}

static int trace_poll_pending(lua_State *L)
{
  TGState *tg = L ? L2TG(L) : NULL;
  return tg && lj_tg_poll_acq(tg) != 0;
}

/* Stitch a new trace to the previous trace. */
uint32_t LJ_FASTCALL lj_trace_stitch_probe(jit_State *J, GCtrace *T)
{
  /*
  ** Stitched fast-function return traces restore across C/VM boundaries where
  ** GC2 stack publication and snapshot reconstruction are still conservative.
  ** Keep these edges interpreted until the stitched-exit state is proven as
  ** complete as ordinary hot-loop exits.
  */
  UNUSED(J); UNUSED(T);
  return 0;
}

void LJ_FASTCALL lj_trace_stitch(jit_State *J, const BCIns *pc, lua_State *L,
				 TraceNo traceno)
{
  UNUSED(L); UNUSED(traceno);
  UNUSED(J); UNUSED(pc);
  return;
}


/* Tiny struct to pass data to protected call. */
typedef struct ExitDataCP {
  jit_State *J;
  lua_State *L;
  void *exptr;		/* Pointer to exit state. */
  GCtrace *T;		/* Exited trace body resolved before flush races. */
  TraceNo parent;	/* Exited trace. */
  ExitNo exitno;	/* Exited snapshot. */
  const BCIns *pc;	/* Restart interpreter at this PC. */
} ExitDataCP;

/* Need to protect lj_snap_restore because it may throw. */
static TValue *trace_exit_cp(lua_State *L, lua_CFunction dummy, void *ud)
{
  ExitDataCP *exd = (ExitDataCP *)ud;
  /* Always catch error here and don't call error function. */
  cframe_errfunc(L->cframe) = 0;
  cframe_nres(L->cframe) = -2*LUAI_MAXSTACK*(int)sizeof(TValue);
#if LJ_TARGET_X64
  exd->pc = lj_snap_restore_exit(exd->J, exd->exptr, exd->L,
				 exd->T, exd->parent, exd->exitno);
#else
  exd->pc = lj_snap_restore(exd->J, exd->exptr);
#endif
  UNUSED(dummy);
  return NULL;
}

#ifndef LUAJIT_DISABLE_VMEVENT
/* Push all registers from exit state. */
static void trace_exit_regs(lua_State *V, ExitState *ex)
{
  int32_t i;
  setintV(V->top++, RID_NUM_GPR);
  setintV(V->top++, RID_NUM_FPR);
  for (i = 0; i < RID_NUM_GPR; i++) {
    if (sizeof(ex->gpr[i]) == sizeof(int32_t))
      setintV(V->top++, (int32_t)ex->gpr[i]);
    else
      setnumV(V->top++, (lua_Number)ex->gpr[i]);
  }
#if !LJ_SOFTFP
  for (i = 0; i < RID_NUM_FPR; i++) {
    setnumV(V->top, ex->fpr[i]);
    if (LJ_UNLIKELY(tvisnan(V->top)))
      setnanV(V->top);
    V->top++;
  }
#endif
}
#endif

#if defined(EXITSTATE_PCREG) || (LJ_UNWIND_JIT && !EXITTRACE_VMSTATE)
/* Determine trace number from pc of exit instruction. */
static TraceNo trace_exit_find(jit_State *J, MCode *pc, GCtrace **Tp)
{
  TraceNo traceno;
  MSize sizetrace = trace_sizetrace_acq(J);
  for (traceno = 1; traceno < sizetrace; traceno++) {
    GCtrace *T = traceref_safe(J, traceno);
    if (trace_exit_body_match(T, traceno)) {
      MCode *mcode = trace_mcode_acq(T);
      MSize szmcode = trace_szmcode_acq(T);
      if (mcode && pc >= mcode && pc < (MCode *)((char *)mcode + szmcode)) {
	if (Tp) *Tp = T;
	return traceno;
      }
    }
  }
  if (Tp) *Tp = NULL;
  lj_assertJ(0, "bad exit pc");
  return 0;
}
#endif

/* A trace exited. Restore interpreter state. */
#if LJ_TARGET_X64 && LJ_ABI_WIN
int LJ_FASTCALL lj_trace_exit(jit_State *J, void *exptr, lua_State *L,
			      uint32_t exitpair)
#elif LJ_TARGET_X64
int LJ_FASTCALL lj_trace_exit(jit_State *J, void *exptr, lua_State *L,
			      TraceNo parent, ExitNo exitno)
#else
int LJ_FASTCALL lj_trace_exit(jit_State *J, void *exptr)
#endif
{
  ERRNO_SAVE
#if LJ_TARGET_X64 && LJ_ABI_WIN
  /* The x64 exit stub encodes both values in 16 bits and GCtrace stores the
  ** corresponding public trace/snapshot counts in 16-bit fields. Keep the
  ** Win64 four-register ABI without publishing either ID through jit_State.
  */
  LJ_STATIC_ASSERT(sizeof(((GCtrace *)0)->traceno) == sizeof(uint16_t));
  LJ_STATIC_ASSERT(sizeof(((GCtrace *)0)->nsnap) == sizeof(uint16_t));
  TraceNo parent = (TraceNo)(exitpair >> 16);
  ExitNo exitno = (ExitNo)(exitpair & 0xffffu);
#elif !LJ_TARGET_X64
  lua_State *L = J->L;
  TraceNo parent = J->parent;
  ExitNo exitno = J->exitno;
#endif
#if LJ_TARGET_X64
  /* Error unwind publishes exitcode in the currently executing TG, not in the
  ** lua_State's migratable owner hint. A coroutine can move between TGs after
  ** that hint was last sampled; use the same TLS/fallback route as unwind.
  */
  TGState *tg = G2TG(G(L));
#endif
  ExitState *ex = (ExitState *)exptr;
  ExitDataCP exd;
  int errcode;
#if LJ_TARGET_X64
  int exitcode = lj_tg_jit_exitcode_acq(tg);
#else
  int exitcode = J->exitcode;
#endif
  TValue exiterr;
  const BCIns *pc;
  void *cf;
  GCtrace *T;
  global_State *g = G(L);

  setnilV(&exiterr);
  if (exitcode) {  /* Trace unwound with error code. */
#if LJ_TARGET_X64
    lj_tg_jit_exitcode_rel(tg, 0);
#else
    J->exitcode = 0;
#endif
    copyTV(L, &exiterr, L->top-1);
  }

#ifdef EXITSTATE_PCREG
  lj_gc2_smr_read_enter(g);
  parent = trace_exit_find(J, (MCode *)(intptr_t)ex->gpr[EXITSTATE_PCREG],
			   &T);
#else
  lj_gc2_smr_read_enter(g);
  UNUSED(ex);
  T = traceref_safe(J, parent);
#endif
  if (!T)
    T = traceref_safe(J, parent);
  lj_assertJ(trace_exit_body_match(T, parent), "bad trace number");
#ifdef EXITSTATE_CHECKEXIT
  if (exitno == trace_nsnap_acq(T)) {  /* Stack check. */
    IRIns base = ir_load_acq(&trace_ir_acq(T)[REF_BASE]);
    lj_assertJ(trace_root_acq(T) != 0, "stack check in root trace");
    exitno = base.op2;
    parent = base.op1;
    T = traceref_safe(J, parent);
    lj_assertJ(trace_exit_body_match(T, parent), "bad stack-check trace");
  }
#endif
  lj_assertJ(trace_exit_body_match(T, parent) && exitno < trace_nsnap_acq(T),
	     "bad trace or exit number");
  exd.J = J;
  exd.L = L;
  exd.exptr = exptr;
  exd.T = T;
  exd.parent = parent;
  exd.exitno = exitno;
  /*
  ** Trace flush/retire keeps stale slots visible until SMR reclamation. Restore
  ** holds a reader while it walks the trace body and snapshot map, otherwise a
  ** concurrent grace pass can free metadata still needed to compute exd.pc.
  */
  errcode = lj_vm_cpcall(L, NULL, &exd, trace_exit_cp);
  lj_gc2_smr_read_leave(g);
  if (errcode) {
    ERRNO_RESTORE
    return -errcode;  /* Return negated error code. */
  }

  if (exitcode) copyTV(L, L->top++, &exiterr);  /* Anchor the error object. */

#if LJ_HASPROFILE
  if (!lj_profile_pending(L))
#endif
    lj_vmevent_send_l(L, TEXIT,
      lj_state_checkstack(V, 4+RID_NUM_GPR+RID_NUM_FPR+LUA_MINSTACK);
      setintV(V->top++, parent);
      setintV(V->top++, exitno);
      trace_exit_regs(V, ex);
    );

  pc = exd.pc;
  cf = cframe_raw(L->cframe);
  setcframe_pc(cf, pc);
  if (exitcode) {
    ERRNO_RESTORE
    return -exitcode;
#if LJ_HASPROFILE
  } else if (lj_profile_pending(L)) {
    /* Just exit to interpreter. */
#endif
  } else {
    int gcdefer = lj_gc2_jit_needs_exit(g);
    if (gcdefer) {
      /* GC-step exits must resume in the interpreter instead of recording a
      ** hot side trace that can stitch back to the same still-due GC check.
      ** Snapshot restore, the TEXIT event and its SMR read section are complete
      ** at this point, so this TG no longer depends on the exiting trace body or
      ** mcode. Publish that quiescence before GC2 starts a handshake: a peer
      ** trace-exit leader may otherwise wait for this jit_base while this TG is
      ** itself waiting to enter the serialized handshake. vm_exit_interp clears
      ** the same field again after this function returns.
      */
#if LJ_TARGET_X64
      lj_tg_store_jit_base(tg, NULL);
#else
      lj_tg_store_jit_base(L2TG(L), NULL);
#endif
      if (gc2_phase_acq(g) == LJ_GC2_MARK)
	(void)lj_gc2_fixpoint_round(g, L, LJ_GC2_WORKER_DRAIN_BATCH);
      if (!(hookmask_load(g) & HOOK_GC))
	lj_gc_step(L);  /* Exited because of GC: drive GC forward. */
    } else if ((jit_flags_acq(J) & JIT_F_ON) && !trace_poll_pending(L)) {
      trace_hotside(J, pc, L, parent, exitno);
    }
  }
  /* Return MULTRES or 0 or -17. */
  ERRNO_RESTORE
  switch (bc_op(*pc)) {
  case BC_CALLM: case BC_CALLMT:
    return (int)((BCReg)(L->top - L->base) - bc_a(*pc) - bc_c(*pc) - LJ_FR2);
  case BC_RETM:
    return (int)((BCReg)(L->top - L->base) + 1 - bc_a(*pc) - bc_d(*pc));
  case BC_TSETM:
    return (int)((BCReg)(L->top - L->base) + 1 - bc_a(*pc));
  case BC_JLOOP: {
    TraceNo targetno = bc_d(*pc);
    GCtrace *target;
    BCIns startins;
    lj_gc2_smr_read_enter(g);
    target = traceref_safe(J, targetno);
    if (!trace_runnable_acq(target, targetno) || trace_startpc_acq(target) != pc) {
      lj_gc2_smr_read_leave(g);
      return 0;  /* Stale JLOOP after a concurrent flush: redispatch it. */
    }
    startins = trace_startins_acq(target);
    lj_gc2_smr_read_leave(g);
    if (bc_isret(bc_op(startins)) || bc_op(startins) == BC_ITERN) {
      /* Dispatch to original ins to ensure forward progress. */
      if (lj_trace_state_load(J) != LJ_TRACE_RECORD) return -17;
      /* Unpatch bytecode when recording. */
      J->patchins = *pc;
      J->patchpc = (BCIns *)pc;
      bc_publish(J->patchpc, startins);
      J->bcskip = 1;
    }
    return 0;
  }
  default:
    if (bc_isfunc_or_ff(bc_op(*pc)))
      return (int)((BCReg)(L->top - L->base) + 1);
    return 0;
  }
}

#if LJ_UNWIND_JIT
#ifdef exitstub_trace_addr
static LJ_AINLINE uintptr_t trace_unwind_exitstub_addr_acq(GCtrace *T,
							   ExitNo exitno)
{
  TraceMCodeView tv;
  tv.mcode = trace_mcode_acq(T);
  tv.szmcode = trace_szmcode_acq(T);
  tv.exitstub = trace_exitstub_acq(T);
  if (tv.mcode == NULL)
    return 0;
#if LJ_TARGET_X86ORX64 && LJ_64
  if (tv.exitstub == NULL)
    return 0;
#endif
  return (uintptr_t)exitstub_trace_addr(&tv, exitno);
}
#endif

/* Given an mcode address determine trace exit address for unwinding. */
uintptr_t LJ_FASTCALL lj_trace_unwind(jit_State *J, uintptr_t addr, ExitNo *ep)
{
  global_State *g = J2G(J);
  GCtrace *T = NULL;
  uintptr_t target = 0;
#if EXITTRACE_VMSTATE
  TGState *tg = J2TG(J);
  TraceNo traceno = tg ?
    (TraceNo)lj_tg_vmstate_load_acq(tg) :
    (TraceNo)vmstate_load_acq(J2G(J));
#else
  TraceNo traceno;
#endif
  MCode *mcode;
  MSize szmcode;
  lj_gc2_smr_read_enter(g);
#if EXITTRACE_VMSTATE
  T = traceref_safe(J, traceno);
#else
  traceno = trace_exit_find(J, (MCode *)addr, &T);
#endif
  if (!T)
    T = traceref_safe(J, traceno);
  mcode = T ? trace_mcode_acq(T) : NULL;
  szmcode = T ? trace_szmcode_acq(T) : 0;
  if (T && mcode
#if EXITTRACE_VMSTATE
      && addr >= (uintptr_t)mcode &&
      addr < (uintptr_t)mcode + szmcode
#endif
     ) {
    SnapShot *snap = trace_snap_acq(T);
    SnapNo lo = 0, exitno = trace_nsnap_acq(T);
    uintptr_t ofs = (uintptr_t)((MCode *)addr - mcode);  /* MCode units! */
    /* Rightmost binary search for mcode offset to determine exit number. */
    do {
      SnapNo mid = (lo+exitno) >> 1;
      if (ofs < snap_mcofs_acq(&snap[mid])) exitno = mid; else lo = mid + 1;
    } while (lo < exitno);
    exitno--;
    *ep = exitno;
#ifdef exitstub_trace_addr
    target = trace_unwind_exitstub_addr_acq(T, exitno);
#elif defined(EXITSTUBS_PER_GROUP)
    target = (uintptr_t)exitstub_addr(J, exitno);
#endif
  }
  lj_gc2_smr_read_leave(g);
  if (target)
    return target;
  /* Cannot correlate addr with trace/exit. This will be fatal. */
  lj_assertJ(0, "bad exit pc");
  return 0;
}
#endif

#endif
