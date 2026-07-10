/*
** Garbage collector.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Major portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lj_gc_c
#define LUA_CORE

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

#define GCSWEEPCOST	10
#define GCACTIVEAUTOSTEPS	64u

#ifdef LJ_GC2_TEST_HELPERS
/*
** Test-only tripwires for the retired color collector. Runtime and shutdown
** counters remain separate so either kind of accidental re-entry is directly
** diagnosable without a debugger. Both classes must stay identically zero.
*/
static GCLegacyEntryStats gc_test_legacy_entries;

static LJ_AINLINE int gc_test_legacy_is_shutdown(global_State *g)
{
  return (g->gc.currentwhite & LJ_GC_SFIXED) != 0;
}

#define gc_test_legacy_count(g, name) \
  ((void)la_add64_rlx(gc_test_legacy_is_shutdown((g)) ? \
			     &gc_test_legacy_entries.shutdown_##name : \
			     &gc_test_legacy_entries.runtime_##name, 1))

void lj_gc_test_legacy_entries_reset(void)
{
  la_store64_rel(&gc_test_legacy_entries.runtime_markobj, 0);
  la_store64_rel(&gc_test_legacy_entries.runtime_markobj_deep, 0);
  la_store64_rel(&gc_test_legacy_entries.runtime_mark, 0);
  la_store64_rel(&gc_test_legacy_entries.runtime_propagate, 0);
  la_store64_rel(&gc_test_legacy_entries.runtime_propagatemark, 0);
  la_store64_rel(&gc_test_legacy_entries.runtime_sweep, 0);
  la_store64_rel(&gc_test_legacy_entries.runtime_sweepstr, 0);
  la_store64_rel(&gc_test_legacy_entries.shutdown_markobj, 0);
  la_store64_rel(&gc_test_legacy_entries.shutdown_markobj_deep, 0);
  la_store64_rel(&gc_test_legacy_entries.shutdown_mark, 0);
  la_store64_rel(&gc_test_legacy_entries.shutdown_propagate, 0);
  la_store64_rel(&gc_test_legacy_entries.shutdown_propagatemark, 0);
  la_store64_rel(&gc_test_legacy_entries.shutdown_sweep, 0);
  la_store64_rel(&gc_test_legacy_entries.shutdown_sweepstr, 0);
}

void lj_gc_test_legacy_entries_snapshot(GCLegacyEntryStats *stats)
{
  if (!stats)
    return;
  stats->runtime_markobj =
    la_load64_acq(&gc_test_legacy_entries.runtime_markobj);
  stats->runtime_markobj_deep =
    la_load64_acq(&gc_test_legacy_entries.runtime_markobj_deep);
  stats->runtime_mark = la_load64_acq(&gc_test_legacy_entries.runtime_mark);
  stats->runtime_propagate =
    la_load64_acq(&gc_test_legacy_entries.runtime_propagate);
  stats->runtime_propagatemark =
    la_load64_acq(&gc_test_legacy_entries.runtime_propagatemark);
  stats->runtime_sweep = la_load64_acq(&gc_test_legacy_entries.runtime_sweep);
  stats->runtime_sweepstr =
    la_load64_acq(&gc_test_legacy_entries.runtime_sweepstr);
  stats->shutdown_markobj =
    la_load64_acq(&gc_test_legacy_entries.shutdown_markobj);
  stats->shutdown_markobj_deep =
    la_load64_acq(&gc_test_legacy_entries.shutdown_markobj_deep);
  stats->shutdown_mark = la_load64_acq(&gc_test_legacy_entries.shutdown_mark);
  stats->shutdown_propagate =
    la_load64_acq(&gc_test_legacy_entries.shutdown_propagate);
  stats->shutdown_propagatemark =
    la_load64_acq(&gc_test_legacy_entries.shutdown_propagatemark);
  stats->shutdown_sweep = la_load64_acq(&gc_test_legacy_entries.shutdown_sweep);
  stats->shutdown_sweepstr =
    la_load64_acq(&gc_test_legacy_entries.shutdown_sweepstr);
}
#else
#define gc_test_legacy_count(g, name) ((void)0)
#endif

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
  GCSize threshold;
  if (mt_live_acq(g) != 0) {
    threshold = lj_gc_mt_threshold_load(g);
    if (mt_live_acq(g) == 0)
      threshold = lj_gc_threshold_load(g);
  } else {
    threshold = lj_gc_threshold_load(g);
  }
  return threshold != LJ_MAX_MEM;
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

/* Macros to set GCobj colors and flags. */
#define white2gray(x)		(lj_obj_cleargcflags((x), LJ_GC_WHITES))
#define gray2black(x)		(lj_obj_addgcflags((x), LJ_GC_BLACK))
#define isfinalized(u)		(lj_obj_gcflags(obj2gco(u)) & LJ_GC_FINALIZED)

static void gc_root_wait_no_l(void)
{
  (void)lj_thr_retry_yield(NULL);
}

static int gc_root_link_valid(global_State *g, GCobj *o);

/* -- Mark phase ---------------------------------------------------------- */

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
    if (lj_tab_array_snapshot_gc(g, t, &array, &asize, &acap) ==
	LJ_TAB_GC_SNAPSHOT_OK && array)
      (void)lj_gc2_markmem(g, acap ? (void *)lj_tab_array_hdrw(array) :
				 (void *)array);
    UNUSED(asize);
    if (lj_tab_node_snapshot_gc(g, t, &node, &hmask) ==
	LJ_TAB_GC_SNAPSHOT_OK && hmask > 0)
      (void)lj_gc2_markmem(g, lj_tab_node_hdrw(node));
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

static void gc_mark(global_State *g, GCobj *o);
static void gc_traverse_thread(global_State *g, lua_State *th);
static void gc_mark_thread_root_tab(global_State *g, GCtab *t);
static void gc_mark_thread_root_proto(global_State *g, GCproto *pt);
static void gc_mark_thread_root_func(global_State *g, GCfunc *fn);
static void gc_mark_thread_root_tv(global_State *g, cTValue *tv);
static void gc_mark_thread_root_upval(global_State *g, GCupval *uv);
static void gc_mark_upval_payload_tv(global_State *g, cTValue *tv);
static void gc_mark_strong_edge_obj(global_State *g, GCobj *o);
static void gc_mark_strong_edge_tv(global_State *g, cTValue *tv);
static int gc2_valid_proto_obj(global_State *g, GCproto *pt);

static int gc_tv_gcref_type_match(global_State *g, cTValue *tv)
{
  GCobj *o;
  if (!tvisgcv(tv))
    return 1;
  o = gcval(tv);
  if (itype(tv) == LJ_TSTR && o == obj2gco(&g->strempty))
    return o->gch.gct == ~LJ_TSTR;
#if LJ_HASFFI
  if (itype(tv) == LJ_TCDATA)
    return lj_gc2_tv_gcref_valid_edge(g, tv);
#endif
  if (o == NULL || !checkptrGC(o) ||
      ((uintptr_t)o & (sizeof(void *) - 1u)) != 0)
    return 0;
  /*
  ** Conservative raw-stack scans can see stale register spill words that look
  ** like collectable TValue tags. Validate the cell through the arena registry
  ** before reading the object header; ordinary live publications initialize the
  ** header before publishing the TValue, so the tag/header match remains the
  ** final semantic edge check.
  */
  if (!lj_gc2_obj_valid(g, o)) {
    /*
    ** Strings are allocated by the plain string path and may not be aligned as
    ** traversable GC-object cells. Prove the containing allocation before
    ** reading the string header. The empty string is embedded in global_State,
    ** but is still a normal, immortal string object for Lua semantics.
    */
    if (itype(tv) != LJ_TSTR ||
	(o != obj2gco(&g->strempty) && !lj_gc2_mem_registered_known(g, o)))
      return 0;
  }
  return ~itype(tv) == o->gch.gct;
}

int lj_gc_tv_gcref_valid(global_State *g, cTValue *tv)
{
  return lj_gc2_tv_gcref_valid_edge(g, tv);
}

static int gc_objroot_gct_valid(global_State *g, GCobj *o, uint32_t *gctp)
{
  uint32_t gct;
  if (!g || !o || !checkptrGC(o))
    return 0;
  if (!lj_gc2_obj_valid_queued(g, o)) {
    /*
    ** Strings may be plain allocations rather than traversable object cells.
    ** For other object roots, queued-object validation must prove the cell
    ** before the header drives the synthetic TValue tag. The empty string is
    ** embedded in global_State and is a valid string root.
    */
    if (o != obj2gco(&g->strempty) && !lj_gc2_mem_registered_known(g, o))
      return 0;
    gct = o->gch.gct;
    if (gct != (uint32_t)~LJ_TSTR)
      return 0;
  } else {
    gct = o->gch.gct;
  }
  if (gct == 0 || gct < (uint32_t)~LJ_TSTR ||
      gct > (uint32_t)~LJ_TUDATA)
    return 0;
  if (gctp)
    *gctp = gct;
  return 1;
}

static int gc_tab_tv_gcref_type_match(global_State *g, cTValue *tv)
{
  GCobj *o;
  if (!tvisgcv(tv))
    return 1;
#if LJ_HASFFI
  if (itype(tv) == LJ_TCDATA)
    return lj_gc2_tv_gcref_valid_edge(g, tv);
#else
  UNUSED(g);
#endif
  o = gcval(tv);
  if (o == NULL || !checkptrGC(o) ||
      ((uintptr_t)o & (sizeof(void *) - 1u)) != 0)
    return 0;
  /*
  ** Table slots are structured array/node snapshots, not conservative raw
  ** stack words. Public full/step GC runs under the public GC exclusive gate
  ** when peers could otherwise enter, so a collectable table slot names an
  ** initialized GC header. Keep the arena-registry validation on raw stack and
  ** root scans, but avoid doing that full registry walk for every table key and
  ** value in large tables.
  */
  return ~itype(tv) == o->gch.gct;
}

static int gc_state_is_sweep(global_State *g)
{
  return g->gc.state == GCSsweepstring || g->gc.state == GCSsweep;
}

/* Mark a TValue (if needed). */
#define gc_marktv(g, tv) \
  { \
    if (gc_tv_gcref_type_match((g), (tv)) && tvisgcv(tv)) { \
      lj_gc_arena_markobj((g), gcV(tv)); \
      if (tviswhite(tv)) gc_mark((g), gcV(tv)); } }

/* Mark a GCobj (if needed). */
#define gc_markobj(g, o) \
  { lj_gc_arena_markobj((g), obj2gco(o)); \
    if (iswhite(obj2gco(o))) gc_mark((g), obj2gco(o)); }

/* Mark a string object. */
#define gc_mark_str(g, s) \
  (lj_gc_arena_markobj((g), obj2gco(s)), \
   lj_obj_cleargcflags(obj2gco(s), LJ_GC_WHITES))

static int gc_mark_rescan_pending_set(global_State *g, GCobj *o)
{
  uint8_t old = la_or8_rlx(&o->gch.marked, LJ_GC_NEEDSCAN);
  if (old & LJ_GC_NEEDSCAN)
    return 0;
  /*
  ** NEEDSCAN is shared by the GC2 rescan queues and color-state handoff. Keep
  ** the pending counters paired with table/thread publications even when the
  ** color-state side is the first publisher, so fixpoint and sweep-close predicates observe
  ** real outstanding work instead of a bare header bit.
  */
  if (o->gch.gct == ~LJ_TTAB)
    gc2_table_rescan_pending_inc(g);
  else if (o->gch.gct == ~LJ_TTHREAD)
    gc2_thread_scan_needscan_pending_inc(g);
  return 1;
}

static void gc_mark_rescan_enqueue(global_State *g, GCobj *o, int force)
{
  if (force) {
    /*
    ** Forced roots are semantic payload roots, not ordinary duplicate edges.
    ** Function/proto/upvalue NEEDSCAN is a same-cycle dedupe stamp: recursive
    ** closure graphs can point back to the closure currently being traversed.
    ** Republishing an already-pending stamp would black-to-gray the same object
    ** again on every self edge and prevent the gray frontier from draining.
    */
    if (!gc_mark_rescan_pending_set(g, o) &&
	(o->gch.gct == ~LJ_TFUNC || o->gch.gct == ~LJ_TPROTO ||
	 o->gch.gct == ~LJ_TUPVAL))
      return;
  } else if (!(lj_obj_gcflags(o) & LJ_GC_NEEDSCAN)) {
    return;
  }
  if (iswhite(o)) {
    gc_mark(g, o);
    return;
  }
  /*
  ** Color gray-list membership is represented by color, as in stock LuaJIT.
  ** The same object link cannot appear on the intrusive gray list twice. Only
  ** the thread that atomically changes black to gray owns the publication.
  */
  gc_mark_active_inc(g);
  if (!lj_gc_claim_black_to_gray(o)) {
    gc_mark_active_dec(g);
    return;
  }
  lj_gc_list_push_rel(&g->gc.gray, o);
  gc_mark_active_dec(g);
}

static void gc_mark_transient_requeue(global_State *g, GCobj *o)
{
  /*
  ** Table resize publishes the retiring bit before all forwarded slots have
  ** reached the successor generation. Color GC must not wait in that window:
  ** requeue the table as ordinary gray work and let the mutator finish the
  ** generation hand-off.
  */
  (void)gc_mark_rescan_pending_set(g, o);
  gc_mark_active_inc(g);
  if (!lj_gc_claim_black_to_gray(o)) {
    gc_mark_active_dec(g);
    return;
  }
  lj_gc_list_push_rel(&g->gc.gray, o);
  gc_mark_active_dec(g);
}

static void gc_mark_needscan_consume(global_State *g, GCobj *o)
{
  uint8_t old;
  /*
  ** Table and thread NEEDSCAN have paired pending counters and represent a
  ** concrete owner handoff, so consuming the color traversal must clear them.
  ** Function/proto/upvalue NEEDSCAN bits are same-cycle dedupe stamps; keep
  ** those until the existing cycle-to-idle cleanup so cyclic closure graphs
  ** cannot force the same prototype back onto the gray list forever. Other
  ** containers, including traces, are ordinary rescan work and must not leave a
  ** stale bit that republishes every already-black back-edge to gray.
  */
  if (o->gch.gct == ~LJ_TTAB) {
    old = la_and8_rlx(&o->gch.marked, (uint8_t)~LJ_GC_NEEDSCAN);
    if (!(old & LJ_GC_NEEDSCAN))
      return;
    if (gc2_table_rescan_pending_acq(g) != 0)
      gc2_table_rescan_pending_dec(g);
  } else if (o->gch.gct == ~LJ_TTHREAD) {
    old = la_and8_rlx(&o->gch.marked, (uint8_t)~LJ_GC_NEEDSCAN);
    if (!(old & LJ_GC_NEEDSCAN))
      return;
    if (gc2_thread_scan_needscan_pending_acq(g) != 0)
      gc2_thread_scan_needscan_pending_dec(g);
  } else if (o->gch.gct == ~LJ_TFUNC || o->gch.gct == ~LJ_TPROTO ||
	     o->gch.gct == ~LJ_TUPVAL) {
    return;
  } else {
    (void)la_and8_rlx(&o->gch.marked, (uint8_t)~LJ_GC_NEEDSCAN);
  }
}

static int gc_grayagain_thread_claim(global_State *g, lua_State *th)
{
  uint32_t cycle = gc2_cycle_acq(g);
  uint32_t old = lj_state_grayagain_cycle_acq(th);
  for (;;) {
    if (old == cycle)
      return 0;
    /*
    ** Threads intentionally remain gray in the color collector, so color is
    ** not a grayagain membership claim. Concurrent root scans and gray
    ** propagation can rediscover the same lua_State in one cycle; the cycle
    ** stamp gives the intrusive gclist link a single publisher.
    */
    if (lj_state_grayagain_cycle_cas(th, &old, cycle))
      return 1;
  }
}

#if LJ_HASFFI
static void gc_mark_clib_cache(global_State *g, CLibrary *cl);
#endif

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

static int gc_udata_payload_valid(GCudata *ud, GCSize *sizep)
{
  MSize len;
  GCSize size;
  uint8_t udtype;
  if (!ud || ud->gct != ~LJ_TUDATA)
    return 0;
  len = ud->len;
  if (len > LJ_MAX_UDATA || len > LJ_MAX_MEM32 - sizeof(GCudata))
    return 0;
  udtype = lj_udata_udtype_acq(ud);
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
      uint32_t cap = la_load32_acq(&ch->cap);
      uint32_t mask = la_load32_acq(&ch->mask);
      uint32_t rendezvous = la_load32_acq(&ch->rendezvous);
      uint64_t bytes;
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

void lj_gc_preserveobj(global_State *g, GCobj *o)
{
  /* SMR-retired GC bodies can outlive their semantic reachability. Preserve
  ** the body itself from GC root list sweep without recursively marking the
  ** object's references; stale lock-free readers may hold this exact body, but
  ** the body is not a root for the Lua object graph.
  */
  if (!g || !o || LJ_UNLIKELY(o->gch.gct == 0))
    return;
  (void)lj_gc2_markobj_nogrey(g, o);
  lj_obj_cleargcflags(o, LJ_GC_WHITES);
  if (o->gch.gct == ~LJ_TFUNC && isluafunc(gco2func(o))) {
    GCfunc *fn = gco2func(o);
    const char *pc = mref(fn->l.pc, const char);
    uint32_t i, nup = lj_funcL_nupvalues(&fn->l);
    /*
    ** A Lua closure body directly contains the executable PC and upvalue refs.
    ** Body-only preservation must therefore keep those direct bodies present for
    ** stale readers and active frame headers; full semantic traversal remains the
    ** job of forced root rescans.
    */
    if (pc && checkptrGC(pc)) {
      GCproto *pt = (GCproto *)(void *)(pc - sizeof(GCproto));
      GCobj *po = obj2gco(pt);
      if (checkptrGC(pt) && lj_gc2_obj_valid(g, po) &&
	  pt->gct == ~LJ_TPROTO) {
	(void)lj_gc2_markobj_nogrey(g, po);
	lj_obj_cleargcflags(po, LJ_GC_WHITES);
      }
    }
    if (nup <= LJ_MAX_UPVAL) {
      for (i = 0; i < nup; i++) {
	GCobj *uv = gcref_acq(fn->l.uvptr[i]);
	if (uv && checkptrGC(uv) && lj_gc2_obj_valid(g, uv) &&
	    uv->gch.gct == ~LJ_TUPVAL) {
	  (void)lj_gc2_markobj_nogrey(g, uv);
	  lj_obj_cleargcflags(uv, LJ_GC_WHITES);
	}
      }
    }
  } else if (o->gch.gct == ~LJ_TTAB) {
    GCtab *t = gco2tab(o);
    TValue *array = lj_tab_array_acq(t);
    Node *node = lj_tab_node_acq(t);
    if (array && !lj_tab_array_is_colocated(t, array))
      (void)lj_gc2_markmem(g, lj_tab_array_hdrw(array));
    if (node && node != &g->nilnode)
      (void)lj_gc2_markmem(g, lj_tab_node_hdrw(node));
  }
}

void lj_gc_markobj(global_State *g, GCobj *o)
{
  if (!g || !o || LJ_UNLIKELY(o->gch.gct == 0))
    return;
  gc_test_legacy_count(g, markobj);
  /*
  ** Active GC2 birth marking can make a proto non-white before constructor
  ** edges queue its traversal. When the color mark bridge is also active,
  ** feed that proto into the color frontier instead of only preserving its
  ** body from sweep.
  */
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
    lj_gc_arena_markobj(g, o);
    if (iswhite(o)) {
      gc_mark(g, o);
    } else if (isgray(o)) {
      return;  /* Already queued on the color frontier. */
    } else {
      gc_mark_rescan_enqueue(g, o, 1);
    }
  } else {
    lj_gc_preserveobj(g, o);
  }
}

static size_t gc_traverse_udata(global_State *g, GCudata *ud)
{
  GCSize size;
  uint8_t udtype;
  GCtab *mt;
  GCtab *env;
  if (LJ_UNLIKELY(!gc_udata_payload_valid(ud, &size)))
    return 0;
  udtype = lj_udata_udtype_acq(ud);
  mt = lj_udata_metatable_acq(ud);
  env = lj_udata_env_acq(ud);
  if (mt) gc_markobj(g, mt);
  if (env) gc_markobj(g, env);
#if LJ_HASFFI
  if (udtype == UDTYPE_FFI_CLIB) {
    CLibrary *cl = (CLibrary *)uddata(ud);
    gc_mark_clib_cache(g, cl);
  }
#endif
  if (LJ_HASBUFFER && udtype == UDTYPE_BUFFER) {
    SBufExt *sbx = (SBufExt *)uddata(ud);
    GCobj *ref;
    if (!sbufiscoworborrow(sbx))
      lj_gc_arena_markmem(g, lj_buf_bptr_acq((SBuf *)sbx));
    ref = lj_bufx_cowref_acq(sbx);
    if (sbufiscow(sbx) && ref)
      gc_markobj(g, ref);
    ref = obj2gco(lj_bufx_dict_str_acq(sbx));
    if (ref)
      gc_markobj(g, ref);
    ref = obj2gco(lj_bufx_dict_mt_acq(sbx));
    if (ref)
      gc_markobj(g, ref);
  }
  if (udtype == UDTYPE_CHANNEL) {
    LJChan *ch = (LJChan *)uddata(ud);
    uint32_t i;
    for (i = 0; i < ch->cap; i++) {
      TValue tv;
      lj_tv_load_acq(&tv, &ch->slot[i].tv);
      gc_marktv(g, &tv);  /* 09 section 9.5 channel slots. */
    }
  }
  if (udtype == UDTYPE_THREAD) {
    LJThread *th = (LJThread *)uddata(ud);
    TValue *roots = lj_thread_start_roots_acq(th);
    uint32_t i, n = lj_thread_start_root_count_acq(th);
    lua_State *child = lj_thread_state_load_acq(th);
    lj_gc_arena_markmem(g, roots);
    if (roots) {
	for (i = 0; i < n; i++) {
	  TValue tv;
	  lj_tv_load_acq(&tv, &roots[i]);
	  gc_mark_thread_root_tv(g, &tv);
	}
      }
    if (child)
      gc_markobj(g, obj2gco(child));  /* 09 section 9.2 child stack. */
  }
  return size;
}

#if LJ_HASFFI
static void gc_mark_clib_cache(global_State *g, CLibrary *cl)
{
  GCtab *cache_env = lj_clib_cache_env_acq(cl);
  CLibCacheEntry *e;
  if (cache_env)
    gc_markobj(g, cache_env);
  for (e = lj_clib_cache_head_acq(cl);
       e != NULL;
       e = lj_clib_cache_next_acq(e)) {
    GCstr *name = lj_clib_cache_name_acq(e);
    TValue tv;
    lj_gc_arena_markmem(g, e);
    if (name)
      gc_mark_str(g, name);
    lj_clib_cache_val_acq(&tv, e);
    gc_marktv(g, &tv);
  }
}
#endif

static int gc_mark_claim_white(global_State *g, GCobj *o)
{
  uint8_t old = la_load8_acq(&o->gch.marked);
  UNUSED(g);
  for (;;) {
    uint8_t next;
    if (!(old & LJ_GC_WHITES))
      return 0;
    /*
    ** Stock LuaJIT has one color marker, so callers assert that an object is
    ** still white after their pre-check. Lockless root publication and helper
    ** barriers can race to the same white object. The first marker atomically
    ** clears the white bits and owns traversal; later markers observe the
    ** already-claimed color and return.
    **
    ** Dead-white resurrection also ends gray after stock flipwhite()+white2gray(),
    ** so clearing the white bits directly preserves the resulting color.
    */
    next = (uint8_t)(old & (uint8_t)~LJ_GC_WHITES);
    if (la_cas8(&o->gch.marked, &old, next, LA_ACQ_REL, LA_ACQ))
      return 1;
  }
}

static GCobj *gc_plain_gcref_acq(const GCRef *ref)
{
  uint64_t u = la_load64_acq(&ref->gcptr64);
  if (u && LJ_UNLIKELY((u & ~LJ_GCVMASK) != 0))
    return NULL;
  return (GCobj *)(uintptr_t)u;
}

static int gc_valid_proto_for_func(global_State *g, GCproto *pt)
{
  MSize minpt;
  UNUSED(g);
  if (!pt || !checkptrGC(pt) || pt->gct != ~LJ_TPROTO)
    return 0;
  if (pt->sizept < sizeof(GCproto) || pt->sizept > LJ_MAX_MEM32)
    return 0;
  if (pt->sizebc == 0 || pt->sizebc > LJ_MAX_BCINS)
    return 0;
  if (pt->framesize > LJ_MAX_SLOTS || pt->sizeuv > LJ_MAX_UPVAL)
    return 0;
  minpt = (MSize)sizeof(GCproto) + pt->sizebc*(MSize)sizeof(BCIns);
  minpt = (minpt + (MSize)sizeof(TValue)-1) & ~((MSize)sizeof(TValue)-1);
  return minpt <= pt->sizept;
}

static int gc_valid_func_obj(global_State *g, GCfunc *fn)
{
  if (!fn || !checkptrGC(fn) ||
      (((uintptr_t)fn & (uintptr_t)(sizeof(void *) - 1u)) != 0) ||
      !lj_gc2_obj_valid(g, obj2gco(fn)) || fn->c.gct != ~LJ_TFUNC)
    return 0;
  if (isluafunc(fn)) {
    GCobj *env = gc_plain_gcref_acq(&fn->l.env);
    const char *pc = mref(fn->l.pc, const char);
    GCproto *pt;
    uint32_t i, nup = lj_funcL_nupvalues(&fn->l);
    if (env && env->gch.gct != ~LJ_TTAB)
      return 0;
    if (nup > LJ_MAX_UPVAL || !pc || !checkptrGC(pc))
      return 0;
    pt = (GCproto *)(void *)(pc - sizeof(GCproto));
    if (!gc_valid_proto_for_func(g, pt) || nup > pt->sizeuv)
      return 0;
    for (i = 0; i < nup; i++) {
      GCobj *uv = gc_plain_gcref_acq(&fn->l.uvptr[i]);
      if (!uv || uv->gch.gct != ~LJ_TUPVAL)
	return 0;
    }
  }
  return 1;
}

/* Mark a white GCobj. */
static void gc_mark(global_State *g, GCobj *o)
{
  int gct = o->gch.gct;
  gc_test_legacy_count(g, mark);
  if (LJ_UNLIKELY(gct == 0))
    return;  /* Body destructor already ran via GC2 arena sweep. */
  if (LJ_UNLIKELY(gct == ~LJ_TFUNC &&
		  !gc_valid_func_obj(g, gco2func(o))))
    return;
  gc_mark_active_inc(g);
  if (LJ_UNLIKELY(!gc_mark_claim_white(g, o))) {
    gc_mark_active_dec(g);
    return;
  }
  lj_gc_arena_markobj(g, o);
  if (LJ_UNLIKELY(gct == ~LJ_TUDATA)) {
    GCudata *ud = gco2ud(o);
    gray2black(o);  /* Userdata are never gray. */
    (void)gc_traverse_udata(g, ud);
  } else if (LJ_UNLIKELY(gct == ~LJ_TUPVAL)) {
    GCupval *uv = gco2uv(o);
    TValue tv;
    lj_tv_load_acq(&tv, uvval(uv));
    gc_mark_upval_payload_tv(g, &tv);
    if (uv->closed)
      gray2black(o);  /* Closed upvalues are never gray. */
  } else if (gct != ~LJ_TSTR && gct != ~LJ_TCDATA) {
    lj_assertG(gct == ~LJ_TFUNC || gct == ~LJ_TTAB ||
	       gct == ~LJ_TTHREAD || gct == ~LJ_TPROTO || gct == ~LJ_TTRACE,
	       "bad GC type %d", gct);
    lj_gc_list_push_rel(&g->gc.gray, o);
  }
  gc_mark_active_dec(g);
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
  return lj_gc2_obj_valid(g, o);
}

static int gc_root_chain_break_cycle(global_State *g, GCobj *head)
{
  GCobj *slow = head, *fast = head, *entry, *tail, *next;
  while (fast != NULL) {
    if (!gc_root_link_valid(g, slow) || !gc_root_link_valid(g, fast))
      return 0;
    slow = lj_obj_gcw_acq(slow);
    fast = lj_obj_gcw_acq(fast);
    if (fast == NULL || slow == NULL)
      return 0;
    if (!gc_root_link_valid(g, fast))
      return 0;
    fast = lj_obj_gcw_acq(fast);
    if (slow == fast)
      break;
  }
  if (fast == NULL)
    return 0;
  slow = head;
  while (slow != fast) {
    if (!gc_root_link_valid(g, slow) || !gc_root_link_valid(g, fast))
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
    if (next == NULL || !gc_root_link_valid(g, next))
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

/* -- Propagation phase --------------------------------------------------- */

static int gc_weak_list_claim(global_State *g, GCtab *t)
{
  /*
  ** Atomic first moves the propagation weak list back to gray, then traverses
  ** those weak tables again so the final weak-clear list sees the completed
  ** atomic liveness frontier. The claim epoch therefore distinguishes the two
  ** publications within one collector cycle.
  */
  uint32_t epoch = (gc2_cycle_acq(g) << 1) |
		   (g->gc.state == GCSatomic ? 1u : 0u);
  uint32_t old = lj_tab_weak_cycle_acq(t);
  for (;;) {
    if (old == epoch)
      return 0;
    /*
    ** Weak tables use the stock intrusive gclist link while waiting for the
    ** atomic weak pass. Walking that list is not a membership claim under
    ** concurrent propagation: another marker can rewrite this table's gclist
    ** between the walk and the push. The per-table publication epoch is the
    ** claim; only the winner may publish the table link for this phase.
    */
    if (lj_tab_weak_cycle_cas(t, &old, epoch))
      return 1;
  }
}

static void gc_mark_tab_edge_obj_checked(global_State *g, GCtab *t)
{
  GCobj *o;
  if (!t)
    return;
  o = obj2gco(t);
  if (LJ_UNLIKELY(o->gch.gct != (uint32_t)~LJ_TTAB))
    return;
  if (iswhite(o)) {
    gc_mark(g, o);
  } else {
    /*
    ** GC2/SMR can make a table body non-white before this color cycle has
    ** traversed its payload. Only an explicit NEEDSCAN handoff means this child
    ** needs another color traversal; ordinary black back-edges are already
    ** covered by stock mark ordering.
    */
    if (lj_obj_gcflags(o) & LJ_GC_NEEDSCAN) {
      lj_gc_arena_markobj(g, o);
      gc_mark_rescan_enqueue(g, o, 0);
    }
  }
}

static void gc_mark_tab_edge_obj(global_State *g, GCtab *t)
{
  GCobj *o;
  if (!t)
    return;
  o = obj2gco(t);
  if (LJ_UNLIKELY(!lj_gc2_obj_valid_queued(g, o)))
    return;
  gc_mark_tab_edge_obj_checked(g, t);
}

static void gc_mark_rescan_edge_obj_checked(global_State *g, GCobj *o,
					    int force)
{
  uint32_t gct;
  if (!o || LJ_UNLIKELY(o->gch.gct == 0))
    return;
  gct = o->gch.gct;
  if (iswhite(o)) {
    gc_mark(g, o);
    return;
  }
  switch (gct) {
  case ~LJ_TTAB:
  case ~LJ_TFUNC:
  case ~LJ_TPROTO:
  case ~LJ_TTHREAD:
  case ~LJ_TUDATA:
  case ~LJ_TUPVAL:
#if LJ_HASJIT
  case ~LJ_TTRACE:
#endif
    /*
    ** Active GC2/SMR preservation can make a child body non-white before this
    ** color cycle has traversed its payload. Queue a normal rescan for
    ** already-marked containers instead of recursively walking functions: closure
    ** graphs can cycle through environments and upvalues, while the gray queue is
    ** already the stock traversal boundary.
    */
    if (force || (lj_obj_gcflags(o) & LJ_GC_NEEDSCAN)) {
      lj_gc_arena_markobj(g, o);
      gc_mark_rescan_enqueue(g, o, force);
    }
    break;
  default:
    break;  /* Strings and cdata have no color payload traversal. */
  }
}

static void gc_mark_rescan_edge_obj(global_State *g, GCobj *o, int force)
{
  if (!o)
    return;
  if (LJ_UNLIKELY(!lj_gc2_obj_valid_queued(g, o))) {
    /*
    ** Stack/upvalue roots can name strings, which are plain string-table
    ** allocations rather than queued traversable GC2 cells. Prove the allocation
    ** before reading the header, then let the checked path clear the white bit.
    */
    if (!lj_gc2_mem_registered_known(g, o) || o->gch.gct != ~LJ_TSTR)
      return;
  }
  gc_mark_rescan_edge_obj_checked(g, o, force);
}

static void gc_mark_strong_edge_obj(global_State *g, GCobj *o)
{
  if (!o || LJ_UNLIKELY(o->gch.gct == 0))
    return;
  if (o->gch.gct == ~LJ_TTAB) {
    gc_mark_tab_edge_obj(g, gco2tab(o));
    return;
  }
  gc_mark_rescan_edge_obj(g, o, 0);
}

static void gc_mark_tab_slot_edge_obj(global_State *g, GCobj *o)
{
  if (!o || LJ_UNLIKELY(o->gch.gct == 0))
    return;
  if (o->gch.gct == ~LJ_TTAB) {
    gc_mark_tab_edge_obj_checked(g, gco2tab(o));
    return;
  }
  gc_mark_rescan_edge_obj_checked(g, o, 0);
}

static void gc_mark_tab_edge_tv(global_State *g, cTValue *tv)
{
  if (LJ_UNLIKELY(!gc_tab_tv_gcref_type_match(g, tv)) || !tvisgcv(tv))
    return;
  gc_mark_tab_slot_edge_obj(g, gcV(tv));
}

static void gc_mark_strong_edge_tv(global_State *g, cTValue *tv)
{
  if (LJ_UNLIKELY(!lj_gc2_tv_gcref_valid_edge(g, tv)) || !tvisgcv(tv))
    return;
  gc_mark_strong_edge_obj(g, gcV(tv));
}

#define GC_TRAVERSE_TAB_TRANSIENT	(-1)

/* Traverse a table. */
static int gc_traverse_tab(global_State *g, GCtab *t)
{
  int weak = 0;
  int ffi_fin = 0;
  TValue *array = NULL;
  MSize asize = 0, acap = 0;
  Node *node = NULL;
  MSize hmask = 0;
  TValue modev;
  cTValue *mode;
  GCtab *mt;
  int array_status, node_status;
  if (LJ_UNLIKELY(!lj_gc2_obj_valid(g, obj2gco(t)) ||
		  obj2gco(t)->gch.gct != (uint32_t)~LJ_TTAB))
    return 0;
  array_status = lj_tab_array_snapshot_gc(g, t, &array, &asize, &acap);
  node_status = lj_tab_node_snapshot_gc(g, t, &node, &hmask);
  if (LJ_UNLIKELY(array_status == LJ_TAB_GC_SNAPSHOT_TRANSIENT ||
		  node_status == LJ_TAB_GC_SNAPSHOT_TRANSIENT))
    return GC_TRAVERSE_TAB_TRANSIENT;
  if (LJ_UNLIKELY(array_status != LJ_TAB_GC_SNAPSHOT_OK ||
		  node_status != LJ_TAB_GC_SNAPSHOT_OK))
    return 0;
  if (array)
    (void)lj_gc2_markmem(g, acap ? (void *)lj_tab_array_hdrw(array) :
				   (void *)array);
  if (hmask > 0)
    (void)lj_gc2_markmem(g, lj_tab_node_hdrw(node));
  mt = lj_tab_metatable_acq(t);
  gc_mark_tab_edge_obj(g, mt);
  mode = lj_meta_fasttv(g, mt, MM_mode, &modev);
  if (mode && tvisstr(mode)) {  /* Valid __mode field? */
    const char *modestr = strVdata(mode);
    int c;
    while ((c = *modestr++)) {
      if (c == 'k') weak |= LJ_GC_WEAKKEY;
      else if (c == 'v') weak |= LJ_GC_WEAKVAL;
    }
    if (weak) {  /* Weak tables are cleared in the atomic phase. */
#if LJ_HASFFI
      if (lj_ctype_fin_istab(g, t)) {
	ffi_fin = 1;
	weak = (int)(~0u & ~LJ_GC_WEAKVAL);
      } else
#endif
      {
	lj_obj_masksetgcflags(obj2gco(t), LJ_GC_WEAK, weak);
	if (gc_weak_list_claim(g, t))
	  lj_gc_list_push_rel(&g->gc.weak, obj2gco(t));
      }
    }
  }
  if (weak == LJ_GC_WEAK)  /* Nothing to mark if both keys/values are weak. */
    return 1;
  if (!(weak & LJ_GC_WEAKVAL)) {  /* Mark array part. */
    MSize i;
    for (i = 0; i < asize; i++) {
      TValue val;
      lj_tv_load_acq(&val, &array[i]);
      if (tvisforward(&val) &&
	  !lj_tab_forwarded_array_slot(t, array, asize, i, &val))
	continue;
      gc_mark_tab_edge_tv(g, &val);
    }
  }
  {  /* Mark hash part. */
    MSize i;
    if (hmask > 0) {
      for (i = 0; i <= hmask; i++) {
	Node *n = &node[i];
	TValue key, val;
	int key_loaded = 0;
	lj_tv_load_acq(&val, &n->val);
#if LJ_HASFFI
	if (ffi_fin) {
	  lj_tv_load_acq(&key, &n->key);
	  key_loaded = 1;
	  while (lj_cdata_fin_isclaim(&val) || tviskeylock(&key)) {
	    gc_root_wait_no_l();
	    lj_tv_load_acq(&val, &n->val);
	    lj_tv_load_acq(&key, &n->key);
	  }
	}
#endif
	if (tvisforward(&val)) {
	  if (!key_loaded) {
	    lj_tv_load_acq(&key, &n->key);
	    key_loaded = 1;
	  }
	  while (tviskeylock(&key)) {
	    gc_root_wait_no_l();
	    lj_tv_load_acq(&key, &n->key);
	  }
	  if (tvisnil(&key) ||
	      !lj_tab_forwarded_hash_slot(t, node, hmask, &key, &val))
	    continue;
	}
	if (!tvisnil(&val)) {  /* Mark non-empty slot. */
	  if (!key_loaded)
	    lj_tv_load_acq(&key, &n->key);
	  lj_assertG(!tvisnil(&key), "mark of nil key in non-empty slot");
	  lj_assertG(!tviskeylock(&key),
		     "mark of key lock in non-empty slot");
	  if (!(weak & LJ_GC_WEAKKEY))
	    gc_mark_tab_edge_tv(g, &key);
	  if (!(weak & LJ_GC_WEAKVAL))
	    gc_mark_tab_edge_tv(g, &val);
	}
      }
    }
  }
  return weak;
}

/* Traverse a function. */
static void gc_traverse_func(global_State *g, GCfunc *fn)
{
  if (LJ_UNLIKELY(!gc_valid_func_obj(g, fn)))
    return;
  {
    GCtab *env = lj_func_env_acq(fn);
    if (env)
      gc_mark_thread_root_tab(g, env);
  }
  if (isluafunc(fn)) {
    uint32_t i, nup = lj_funcL_nupvalues(&fn->l);
    lj_assertG(nup <= funcproto(fn)->sizeuv,
	       "function upvalues out of range");
    gc_mark_thread_root_proto(g, funcproto(fn));
    for (i = 0; i < nup; i++)  /* Mark Lua function upvalues. */
      gc_mark_thread_root_upval(g, func_uv_acq(&fn->l, i));
  } else {
    uint32_t i, nup = lj_funcC_nupvalues(&fn->c);
    for (i = 0; i < nup; i++) {  /* Mark C function upvalues. */
      TValue tv;
      lj_tv_load_acq(&tv, &fn->c.upvalue[i]);
      /*
      ** C closure upvalues are strong function payload edges. A child table can
      ** already be non-white because GC2/SMR preserved its body before this
      ** color propagation reached the closure, so use the same strong-edge path
      ** as local cells: mutable containers are rescanned, while non-white
      ** functions are queued through the gray list to avoid closure-cycle
      ** recursion.
      */
      gc_mark_strong_edge_tv(g, &tv);
    }
  }
}

#if LJ_HASJIT
static GCtrace *gc_traceref_safe(global_State *g, TraceNo traceno)
{
  return traceref_safe(G2J(g), traceno);
}

static int gc_trace_geometry_valid(GCtrace *T)
{
  IRIns *irbase;
  SnapShot *snap;
  SnapEntry *snapmap;
  IRRef nins, nk;
  SnapNo nsnap;
  MSize nsnapmap;
  const MSize snapmap_per_snap =
    (MSize)LJ_MAX_JSLOTS + (MSize)LJ_STACK_EXTRA + 32u;
  if (!T || !checkptrGC(T) || T->gct != (uint32_t)~LJ_TTRACE)
    return 0;
  nins = trace_nins_acq(T);
  nk = trace_nk_acq(T);
  nsnap = trace_nsnap_acq(T);
  nsnapmap = trace_nsnapmap_acq(T);
  if (nins < REF_BASE || nins > 0xffffu || nk > REF_BIAS || nk > nins)
    return 0;
  if ((nsnap == 0 && nsnapmap != 0) ||
      (nsnap != 0 && nsnapmap > (MSize)nsnap * snapmap_per_snap))
    return 0;
  irbase = trace_ir_acq(T);
  if (nk < REF_TRUE && (!irbase || !checkptrGC(&irbase[nk])))
    return 0;
  snap = trace_snap_acq(T);
  snapmap = trace_snapmap_acq(T);
  if (nsnap == 0)
    return 1;
  return snap && snapmap && checkptrGC(snap) && checkptrGC(snapmap);
}

/* Mark a trace. */
static void gc_marktrace(global_State *g, TraceNo traceno)
{
  GCtrace *T = gc_traceref_safe(g, traceno);
  GCobj *o;
  if (!T)
    return;
  o = obj2gco(T);
  lj_assertG(traceno != G2J(g)->cur.traceno, "active trace escaped");
  gc_mark(g, o);
}

void lj_gc_mark_trace_slot(global_State *g, uint32_t traceno)
{
  gc_marktrace(g, (TraceNo)traceno);
}

typedef struct GCTraceProtoPCCache {
  const BCIns *start;
  const BCIns *end;
  GCobj *o;
} GCTraceProtoPCCache;

#define GC_TRACE_PROTO_PC_CACHE		256
#define GC_TRACE_PROTO_PC_WALK_BUDGET	8192u

static GCproto *gc_trace_pc_proto_candidate(global_State *g, GCobj *o,
					    const BCIns **bcp,
					    const BCIns **endp)
{
  GCproto *pt;
  const BCIns *bc;
  uint32_t gct;
  if (!gc_objroot_gct_valid(g, o, &gct) || gct != (uint32_t)~LJ_TPROTO)
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

int lj_gc_test_trace_pc_proto_candidate(global_State *g, GCobj *o,
					const BCIns *pc)
{
  const BCIns *bc, *end;
  return pc && gc_trace_pc_proto_candidate(g, o, &bc, &end) &&
	 pc >= bc && pc < end;
}

static void gc_mark_proto_for_trace_pc(global_State *g, const BCIns *pc,
				       GCTraceProtoPCCache *cache,
				       MSize *ncachep, uint32_t *budgetp)
{
  GCobj *o;
  MSize i, ncache = *ncachep;
  if (!pc || !budgetp || *budgetp == 0)
    return;
  for (i = 0; i < ncache; i++) {
    if (pc >= cache[i].start && pc < cache[i].end) {
      gc_mark_thread_root_proto(g, gco2pt(cache[i].o));
      return;
    }
  }
  for (o = lj_gc_root_acq(g); o != NULL && *budgetp != 0;) {
    GCobj *next;
    const BCIns *bc, *end;
    GCproto *pt;
    if (LJ_UNLIKELY(!gc_root_link_valid(g, o)))
      break;
    next = lj_obj_gcw_acq(o);
    --*budgetp;
    pt = gc_trace_pc_proto_candidate(g, o, &bc, &end);
    if (pt) {
      /*
      ** Snapshot PC ownership is advisory trace metadata retention. Bound the
      ** total root-spine search for one trace and cache proto bytecode ranges
      ** already seen, so traces with many snapshots cannot turn a GC step into
      ** repeated full root walks.
      */
      if (ncache < GC_TRACE_PROTO_PC_CACHE) {
	cache[ncache].start = bc;
	cache[ncache].end = end;
	cache[ncache].o = o;
	*ncachep = ++ncache;
      }
      if (pc >= bc && pc < end) {
	gc_mark_thread_root_proto(g, pt);
	return;
      }
    }
    if (next == o)
      break;
    o = next;
  }
}

static void gc_mark_trace_snapshot_pcs(global_State *g, GCtrace *T)
{
  SnapShot *snap = trace_snap_acq(T);
  SnapEntry *snapmap = trace_snapmap_acq(T);
  MSize nsnapmap = trace_nsnapmap_acq(T);
  SnapNo i, nsnap = trace_nsnap_acq(T);
  GCTraceProtoPCCache cache[GC_TRACE_PROTO_PC_CACHE];
  MSize ncache = 0;
  uint32_t walk_budget = GC_TRACE_PROTO_PC_WALK_BUDGET;
  (void)lj_gc_flush_root_pending(g);
  (void)lj_gc_repair_root_spine(g);
  for (i = 0; i < nsnap; i++) {
    SnapShot *s = &snap[i];
    MSize ofs = snap_mapofs_acq(s);
    MSize nent = snap_nent_acq(s);
    SnapEntry *map;
    if (ofs >= nsnapmap || nent >= nsnapmap - ofs)
      return;
    map = &snapmap[ofs];
    gc_mark_proto_for_trace_pc(g, snap_pc_acq(&map[nent]), cache, &ncache,
			       &walk_budget);
  }
}

static void gc_mark_active_cframe_proto(global_State *g, lua_State *th)
{
  void *cf;
  const BCIns *pc;
  GCTraceProtoPCCache cache[GC_TRACE_PROTO_PC_CACHE];
  MSize ncache = 0;
  uint32_t walk_budget = GC_TRACE_PROTO_PC_WALK_BUDGET;
  if (!th || th->cframe == NULL)
    return;
  cf = cframe_raw(th->cframe);
  pc = cf ? cframe_pc(cf) : NULL;
  if (!pc)
    return;
  (void)lj_gc_flush_root_pending(g);
  (void)lj_gc_repair_root_spine(g);
  gc_mark_proto_for_trace_pc(g, pc, cache, &ncache, &walk_budget);
}

/* Traverse a trace. */
static void gc_traverse_trace(global_State *g, GCtrace *T)
{
  IRIns *irbase;
  IRRef ref;
  if (trace_traceno_acq(T) == 0) return;
  if (T != &G2J(g)->cur && !gc_trace_geometry_valid(T)) return;
  irbase = trace_ir_acq(T);
  for (ref = trace_nk_acq(T); ref < REF_TRUE; ref++) {
    IRIns *ir = &irbase[ref];
    IRIns irs = ir_load_acq(ir);
    if (irs.o == IR_KGC)
      gc_mark_strong_edge_obj(g, ir_kgc_load_acq(ir));
    if (irt_is64(irs.t) && irs.o != IR_KNULL)
      ref++;
  }
  {
    TraceNo link = trace_link_acq(T);
    TraceNo nextroot = trace_nextroot_acq(T);
    TraceNo nextside = trace_nextside_acq(T);
    if (link) gc_marktrace(g, link);
    if (nextroot) gc_marktrace(g, nextroot);
    if (nextside) gc_marktrace(g, nextside);
  }
  gc_mark_rescan_edge_obj(g, trace_startptgco_acq(T), 1);
  gc_mark_trace_snapshot_pcs(g, T);
}

#endif

/* Traverse a prototype. */
static void gc_traverse_proto(global_State *g, GCproto *pt)
{
  ptrdiff_t i;
  gc_mark_str(g, proto_chunkname_acq(pt));
  for (i = -(ptrdiff_t)pt->sizekgc; i < 0; i++)  /* Mark collectable consts. */
    gc_mark_strong_edge_obj(g, proto_kgc_acq(pt, i));
#if LJ_HASJIT
  {
    TraceNo trace = proto_trace_acq(pt);
    if (trace) gc_marktrace(g, trace);
  }
#endif
}

static GCproto *gc_func_proto_if_lua(GCfunc *fn)
{
  return fn->c.ffid == FF_LUA ?
	 (GCproto *)(mref(fn->l.pc, char) - sizeof(GCproto)) : NULL;
}

static int gc_frame_func_valid(global_State *g, TValue *frame,
			       GCfunc **fnp, GCproto **ptp)
{
  cTValue *ftv = frame - 1;
  GCobj *fo;
  GCfunc *fn;
  if (fnp) *fnp = NULL;
  if (ptp) *ptp = NULL;
  if (!tvisfunc(ftv))
    return 0;
  fo = frame_gc(frame);
  if (!fo || !checkptrGC(fo) ||
      (((uintptr_t)fo & (uintptr_t)(sizeof(void *) - 1u)) != 0) ||
      !lj_gc2_obj_valid(g, fo) || fo->gch.gct != ~LJ_TFUNC)
    return 0;
  fn = &fo->fn;
  if (!gc_valid_func_obj(g, fn))
    return 0;
  if (isluafunc(fn)) {
    const char *pc = mref(fn->l.pc, const char);
    GCproto *pt = (GCproto *)(void *)(pc - sizeof(GCproto));
    if (!gc_valid_proto_for_func(g, pt))
      return 0;
    if (ptp) *ptp = pt;
  }
  if (fnp) *fnp = fn;
  return 1;
}

static int gc_frame_prev_safe(global_State *g, TValue *bot, TValue *max,
			      TValue *frame, TValue **prevp, GCfunc **fnp)
{
  GCfunc *fn = NULL;
  GCproto *pt = NULL;
  TValue *prev;
  if (prevp) *prevp = NULL;
  if (fnp) *fnp = NULL;
  if (frame <= bot + LJ_FR2 || frame >= max)
    return 0;
  (void)gc_frame_func_valid(g, frame, &fn, &pt);
  if (frame_islua(frame)) {
    const BCIns *pc, *bc;
    if (!pt)
      return 0;
    pc = frame_pc(frame);
    bc = proto_bc(pt);
    if (!pc || pc <= bc || pc > bc + pt->sizebc)
      return 0;
    prev = frame - (1 + LJ_FR2 + bc_a(pc[-1]));
  } else {
    ptrdiff_t sz = frame_sized(frame);
    if (sz <= 0 || (uintptr_t)sz > (uintptr_t)((char *)frame -
						(char *)(bot + LJ_FR2)))
      return 0;
    prev = (TValue *)((char *)frame - sz);
  }
  if (prevp) *prevp = prev;
  if (fnp) *fnp = fn;
  return 1;
}

#if LJ_HASJIT
static TValue *gc_thread_jit_base(global_State *g, lua_State *th)
{
  TGState *tg = lj_tg_thread_active(g, th);
  return tg ? lj_tg_load_jit_base(tg) : NULL;
}
#endif

#if LJ_HASJIT
static void gc_mark_jit_frame_funcs(global_State *g, lua_State *th)
{
  TValue *base = gc_thread_jit_base(g, th);
  TValue *bot, *max, *frame;
  uint32_t n = 0;
  if (!base || !th || tvref(th->stack) == NULL)
    return;
  bot = tvref(th->stack);
  max = tvref(th->maxstack);
  if (base <= bot + 1 + LJ_FR2 || base > max)
    return;
  /*
  ** JIT C helpers keep jit_base published while vmstate is C. The raw stack
  ** scan below keeps value slots live, but frame headers are not normal tagged
  ** values. Mark their function/prototype chain explicitly.
  */
  for (frame = base - 1; frame > bot + LJ_FR2 && frame < max; ) {
    GCfunc *fn;
    TValue *prev;
    if (!gc_frame_prev_safe(g, bot, max, frame, &prev, &fn))
      break;
    if (fn)
      gc_mark_thread_root_func(g, fn);
    if (prev >= frame || prev <= bot + LJ_FR2 || prev >= max)
      break;
    frame = prev;
    if (++n >= LJ_GC2_ROOT_SCAN_LIMIT)
      break;
  }
}
#endif

static int gc_thread_is_remote_current(global_State *g, lua_State *th)
{
  uint32_t owner;
  TGState *tg;
  if (!g || !th)
    return 0;
  owner = lj_state_owner_acq(th);
  if (owner == 0 || owner == LJ_THREAD_GCSCAN)
    return 0;
  tg = lj_tg_find_owner(g, owner);
  return tg && !lj_tg_flags_test_acq(tg, TGF_DEAD) &&
	 lj_tg_load_cur_L(tg) == th && th != lj_tg_cur_L(g);
}

static int gc_thread_is_native_current(global_State *g, lua_State *th)
{
  TGState *tg = lj_tg_thread_active(g, th);
  /*
  ** Native boundaries can acknowledge safepoints before the VM has returned
  ** through the C frame. Preserve the raw stack range there: frame headers are
  ** not ordinary TValue slots, and the owner may still rely on the exact frame
  ** chain when the native call resumes.
  */
  return tg && lj_tg_in_native_acq(tg) != 0;
}

static int gc_thread_is_jit_current(global_State *g, lua_State *th)
{
#if LJ_HASJIT
  /*
  ** Trace/native helpers own the current frame layout until jit_base and the
  ** positive trace vmstate are cleared. Color root marking mirrors GC2 here:
  ** preserve raw stack storage and explicitly preserve frame-header function
  ** roots when jit_base is the only published edge.
  */
  {
    TGState *tg = lj_tg_thread_active(g, th);
    return tg && lj_tg_jit_active_acq(tg);
  }
#else
  UNUSED(g); UNUSED(th);
  return 0;
#endif
}

static void gc_mark_frame_chain_funcs(global_State *g, lua_State *th)
{
  TValue *bot, *max, *frame;
  uint32_t n = 0;
  if (!th || tvref(th->stack) == NULL)
    return;
  bot = tvref(th->stack);
  max = tvref(th->maxstack);
  frame = th->base - 1;
  /*
  ** Raw stack preservation scans TValue slots, but frame headers are not ordinary
  ** slots on every backend/state transition. Remote, native and recorder-owned
  ** states therefore also need a bounded header walk to keep live closure payloads
  ** and their prototypes rooted until the owner publishes a precise scan.
  */
  while (frame > bot + LJ_FR2 && frame < max) {
    GCfunc *fn;
    TValue *prev;
    if (!gc_frame_prev_safe(g, bot, max, frame, &prev, &fn))
      break;
    if (fn)
      gc_mark_thread_root_func(g, fn);
    if (prev >= frame || prev <= bot + LJ_FR2 || prev >= max)
      break;
    frame = prev;
    if (++n >= LJ_GC2_ROOT_SCAN_LIMIT)
      break;
  }
}

/* Traverse the frame structure of a stack. */
static MSize gc_traverse_frames(global_State *g, lua_State *th)
{
  TValue *frame, *top = th->top-1, *bot = tvref(th->stack);
  /* Note: extra vararg frame not skipped, marks function twice (harmless). */
  for (frame = th->base-1; frame > bot+LJ_FR2; frame = frame_prev(frame)) {
    GCfunc *fn = frame_func(frame);
    GCproto *pt = gc_func_proto_if_lua(fn);
    TValue *ftop = frame;
    if (pt) ftop += pt->framesize;
    if (ftop > top) top = ftop;
    gc_mark_thread_root_func(g, fn);
  }
  top++;  /* Correct bias of -1 (frame == base-1). */
  if (top > tvref(th->maxstack)) top = tvref(th->maxstack);
  return (MSize)(top - bot);  /* Return minimum needed stack size. */
}

static BCReg gc_live_local_topslot(GCproto *pt, const BCIns *ip)
{
  const char *p = (const char *)proto_varinfo(pt);
  BCPos pc, lastpc = 0;
  BCReg nactive = 0;
  if (!p)
    return pt->framesize;
  pc = proto_bcpos(pt, ip);
  for (;;) {
    uint32_t vn = *(const uint8_t *)p;
    BCPos startpc, endpc;
    if (vn < VARNAME__MAX) {
      if (vn == VARNAME_END)
	break;
    } else {
      do { p++; } while (*(const uint8_t *)p);
    }
    p++;
    lastpc = startpc = lastpc + lj_buf_ruleb128(&p);
    if (startpc > pc)
      break;
    endpc = startpc + lj_buf_ruleb128(&p);
    if (pc < endpc)
      nactive++;
  }
  return nactive < pt->framesize ? nactive : pt->framesize;
}

/* Calculate number of used slots in a live bytecode frame. */
static BCReg gc_cur_topslot(GCproto *pt, const BCIns *pc, uint32_t nres,
			     int *precisep)
{
  BCIns ins = pc[-1];
  if (precisep)
    *precisep = 0;
  if (bc_op(ins) == BC_UCLO)
    ins = pc[bc_j(ins)];
  switch (bc_op(ins)) {
  case BC_CALLM: case BC_CALLMT:
    /*
    ** Multi-result calls depend on the VM MULTRES register, which is not always
    ** a reliable cframe source for asynchronous root snapshots. Keep the whole
    ** frame for these variable-result windows.
    */
    return pt->framesize;
  case BC_CALL:
  case BC_ITERC:
    /*
    ** Fixed-argument calls have a stable bytecode call window, but true locals
    ** can live above that window and later be captured by FNEW. Merge the call
    ** slots with debug local ranges so stale temporaries remain collectible
    ** without letting live locals go dead. Stripped prototypes fall back to the
    ** full frame because there is no precise local map.
    */
    if (bc_c(ins) != 0) {
      BCReg top = bc_a(ins) + bc_c(ins) + LJ_FR2;
      BCReg ltop = gc_live_local_topslot(pt, pc-1);
      if (ltop > top)
	top = ltop;
      if (precisep)
	*precisep = top < pt->framesize;
      return top;
    }
    break;
  case BC_RETM:
    return bc_a(ins) + bc_d(ins) + nres-1;
  case BC_TSETM:
    /*
    ** TSETM consumes the same variable-result window; keep the frame live until
    ** the VM table-store helper publishes the payload range.
    */
    return pt->framesize;
  default:
    return pt->framesize;
  }
  return pt->framesize;
}

static TValue *gc_active_thread_top(global_State *g, lua_State *th, TValue *top,
				    int *precisep)
{
  TValue *bot = tvref(th->stack);
  TValue *max = tvref(th->maxstack);
  TValue *frame;
  if (precisep)
    *precisep = 0;
  if (top > max)
    top = max;
  if (th->base <= bot + 1 + LJ_FR2)
    return top;
  frame = th->base - 1;
  if (frame > bot + LJ_FR2 && frame < max && frame_islua(frame)) {
    GCfunc *fn;
    GCproto *pt;
    (void)gc_frame_func_valid(g, frame, &fn, &pt);
    if (pt) {
      TValue *ltop = th->base + pt->framesize;
      if (ltop > top)
	top = ltop;
    }
  } else if (frame > bot + LJ_FR2 && frame < max && frame_isc(frame)) {
    TValue *prev;
    GCfunc *fn;
    if (gc_frame_prev_safe(g, bot, max, frame, &prev, &fn) &&
	prev > bot + LJ_FR2 && prev < max && frame_islua(prev)) {
      GCproto *pt;
      (void)gc_frame_func_valid(g, prev, &fn, &pt);
      if (pt) {
	void *cf = cframe_raw(th->cframe);
	const BCIns *pc = cf ? cframe_pc(cf) : NULL;
	const BCIns *bc = proto_bc(pt);
	if (pc && pc > bc && pc <= bc + pt->sizebc) {
	  /*
	  ** The bytecode call window is the precise same-thread C-call root set.
	  ** Open local cells are scanned through the thread's open-upvalue list;
	  ** broadening every cell-op frame here keeps stale temporaries alive and
	  ** changes stock weak/trace collection semantics.
	  */
	  TValue *ctop = prev + 1 + gc_cur_topslot(pt, pc,
						   cframe_multres_n(cf),
						   precisep);
	  if (ctop > top)
	    top = ctop;
	}
      }
    }
  }
  return top > max ? max : top;
}

static void gc_mark_thread_root_proto(global_State *g, GCproto *pt)
{
  GCobj *o;
  if (!pt)
    return;
  o = obj2gco(pt);
  if (gc_state_is_sweep(g)) {
    lj_gc_markobj_deep(g, o);
    return;
  }
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
    lj_gc_arena_markobj(g, o);
    if (iswhite(o)) {
      gc_mark(g, o);
    } else {
      /*
      ** Prototype constants are immutable semantic payload of a live frame
      ** function. A black/gray proto can carry a stale NEEDSCAN bit after a lost
      ** intrusive-list handoff; direct traversal here keeps nested function
      ** literals live without depending on duplicate gray publication.
      */
      gc_traverse_proto(g, pt);
    }
    return;
  }
  /*
  ** Function-owned prototypes are payload roots for nested FNEW prototypes and
  ** trace metadata. GC2/SMR can preserve or color the prototype body before the
  ** color collector reaches the owning closure; the color pass still has to
  ** traverse the prototype constants for this cycle so a child prototype cannot
  ** be swept before its closure is created.
  */
  gc_mark_rescan_edge_obj(g, o, 1);
}

static void gc_mark_upval_payload_tv(global_State *g, cTValue *tv)
{
  GCobj *o;
  if (LJ_UNLIKELY(!gc_tv_gcref_type_match(g, tv)) || !tvisgcv(tv))
    return;
  o = gcV(tv);
  if (LJ_UNLIKELY(o->gch.gct == 0))
    return;
  if (LJ_UNLIKELY(itype(tv) == LJ_TUPVAL)) {
    gc_mark_thread_root_upval(g, gco2uv(o));
    return;
  }
  if (LJ_UNLIKELY(o->gch.gct == ~LJ_TFUNC)) {
    gc_mark_rescan_edge_obj(g, o, 1);
    return;
  }
  gc_mark_strong_edge_obj(g, o);
}

static void gc_mark_thread_root_upval(global_State *g, GCupval *uv)
{
  GCobj *o;
  TValue tv;
  if (!uv || uv->gct != ~LJ_TUPVAL)
    return;
  o = obj2gco(uv);
  if (gc_state_is_sweep(g)) {
    lj_gc_markobj_deep(g, o);
    return;
  }
  lj_gc_arena_markobj(g, o);
  if (iswhite(o)) {
    gc_mark(g, o);
    return;
  }
  /*
  ** Local-cell stack slots are roots whose GC object may have been preserved by
  ** arena/SMR state before this color cycle saw the slot. Color alone then only
  ** proves the cell body survives; the contained Lua value is still the semantic
  ** root and must be sampled for this collection.
  */
  lj_tv_load_acq(&tv, uvval(uv));
  if (LJ_UNLIKELY(tvisgcv(&tv) && gcV(&tv) == o))
    return;
  gc_mark_upval_payload_tv(g, &tv);
}

static void gc_mark_thread_root_func(global_State *g, GCfunc *fn)
{
  GCobj *o;
  if (!fn || LJ_UNLIKELY(!gc_valid_func_obj(g, fn)))
    return;
  o = obj2gco(fn);
  if (gc_state_is_sweep(g)) {
    lj_gc_markobj_deep(g, o);
    return;
  }
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
    lj_gc_arena_markobj(g, o);
    if (iswhite(o)) {
      gc_mark(g, o);
    } else {
      /*
      ** Frame headers are executable state. If the closure body is already
      ** non-white, traverse the payload immediately so its prototype/upvalue graph
      ** does not rely on a same-object gray-list handoff that may have been
      ** consumed by another publisher.
      */
      gc_traverse_func(g, fn);
    }
    return;
  }
  /*
  ** Stack frame functions are primary roots for their prototype graph. White
  ** closures enter the ordinary gray frontier; already-marked closures are still
  ** rescanned here because frame headers are not ordinary TValue slots. A closure
  ** body can be preserved or blackened before the current root snapshot reaches
  ** the live frame, but the frame still semantically roots its proto/upvalues.
  */
  gc_mark_rescan_edge_obj(g, o, 1);
}

static void gc_mark_thread_root_tv(global_State *g, cTValue *tv)
{
  GCobj *o;
  if (LJ_UNLIKELY(!gc_tv_gcref_type_match(g, tv)) || !tvisgcv(tv))
    return;
  o = gcV(tv);
  if (LJ_UNLIKELY(o->gch.gct == 0))
    return;
  if (gc_state_is_sweep(g)) {
    lj_gc_markobj_deep(g, o);
    return;
  }
  if (LJ_UNLIKELY(itype(tv) == LJ_TUPVAL)) {
    gc_mark_thread_root_upval(g, gco2uv(o));
    return;
  }
  if (LJ_UNLIKELY(o->gch.gct == ~LJ_TFUNC)) {
    /*
    ** Closure-valued stack roots are roots for the closure payload, not just the
    ** closure header. GC2/SMR can color the closure before color propagation sees
    ** this slot, so force a gray-list rescan to retain the prototype/upvalue graph.
    */
    gc_mark_rescan_edge_obj(g, o, 1);
    return;
  }
  gc_mark_rescan_edge_obj(g, o, 0);
}

static void gc_mark_thread_root_tab(global_State *g, GCtab *t)
{
  if (!t)
    return;
  if (gc_state_is_sweep(g)) {
    lj_gc_markobj_deep(g, obj2gco(t));
    return;
  }
  gc_mark_rescan_edge_obj(g, obj2gco(t), 0);
}

/* Traverse a thread object. */
static void gc_traverse_thread(global_State *g, lua_State *th)
{
  GCobj *mt;
  TValue *o, *top;
  TValue tv;
  MSize used;
  uint32_t owner;
  int owned;
  int remote_current;
  int native_current;
  int jit_current;
  lua_State *cur_L;
  /*
  ** Thread stack storage is a raw allocation: it is not reachable through the
  ** GC object header once a racy owner/resizer publishes a transient state.
  ** Validate the stack/header contract before touching the raw allocation.
  */
  if (LJ_UNLIKELY(!lj_gc2_valid_thread_for_traverse(g, th)))
    return;
  top = th->top;
  lj_gc_arena_markmem(g, tvref(th->stack));
  cur_L = lj_tg_cur_L(g);
  owner = lj_state_owner_acq(th);
  owned = owner != 0 && owner != LJ_THREAD_GCSCAN && th != cur_L;
  remote_current = gc_thread_is_remote_current(g, th);
  native_current = gc_thread_is_native_current(g, th);
  jit_current = gc_thread_is_jit_current(g, th);
  if (owned || remote_current || native_current || jit_current) {
    gc_mark_frame_chain_funcs(g, th);
#if LJ_HASJIT
    if (jit_current)
      gc_mark_jit_frame_funcs(g, th);
#endif
    /*
    ** Remote, native and JIT-owned frame chains are owner-private until their
    ** boundary publishes a stable base/top pair. Preserve raw stack storage in
    ** that window; ordinary same-thread VM collections use precise frame tops
    ** so stale slots do not keep weak entries alive.
    */
    top = tvref(th->maxstack);
    used = (MSize)(top - tvref(th->stack));
  } else {
    used = gc_traverse_frames(g, th);
  }
  if (th == cur_L && th->cframe != NULL) {
#if LJ_HASJIT
    gc_mark_active_cframe_proto(g, th);
#endif
  }
  if (!remote_current && !native_current && !jit_current && th == cur_L &&
      th->base > tvref(th->stack) + 1 + LJ_FR2) {
    top = gc_active_thread_top(g, th, top, NULL);
    /*
    ** The PC-derived top bounds the current C-call window. Frame functions were
    ** already marked above, and open local cells are scanned through the open-upval
    ** list; widening back to the declared frame size revives dead lexical slots.
    */
  } else if (tvref(th->stack) + used > top) {
    top = tvref(th->stack) + used;
  }
  for (o = tvref(th->stack)+1+LJ_FR2; o < top; o++) {
    lj_tv_load_acq(&tv, o);
    gc_mark_thread_root_tv(g, &tv);
  }
  if (!owned && !remote_current && !native_current && !jit_current &&
      g->gc.state == GCSatomic) {
    top = tvref(th->stack) + th->stacksize;
    for (; o < top; o++)  /* Clear unmarked slots. */
      setnilV(o);
  }
  for (mt = lj_state_openupval_acq(th); mt != NULL; mt = lj_obj_gcw_acq(mt)) {
    if (LJ_LIKELY(mt->gch.gct == ~LJ_TUPVAL))
      gc_mark_thread_root_upval(g, gco2uv(mt));
  }
  {
    GCtab *env = lj_state_env_acq(th);
    gc_mark_thread_root_tab(g, env);
  }
  mt = obj2gco(lj_thread_state_udata_acq(g, th));
  if (mt != NULL)
    gc_markobj(g, mt);
  if (!owned && th != cur_L)
    lj_state_shrinkstack(th, used);
}

/* Propagate one gray object. Traverse it and turn it black. */
static size_t propagatemark(global_State *g)
{
  GCobj *o = lj_gc_list_head_acq(&g->gc.gray);
  int gct;
  int black_rescan;
  size_t m = 0;
  gc_test_legacy_count(g, propagatemark);
  if (o == NULL)
    return 0;
  gc_mark_active_inc(g);
  if (LJ_UNLIKELY(!lj_gc2_obj_valid_queued(g, o))) {
    /*
    ** A stale duplicate can survive on the lock-free gray list after its arena
    ** cell has been reclaimed. Drop the list head before reading object color or
    ** type; arena validation is the only safe predicate for such candidates.
    */
    lj_gc_list_pop_head_rel(&g->gc.gray, o);
    goto done;
  }
  gct = o->gch.gct;
  black_rescan = !isgray(o) && isblack(o);
  if (LJ_UNLIKELY(!isgray(o) && !black_rescan)) {
    /*
    ** Lock-free publication can leave duplicate/stale nodes on the color gray
    ** list after another path has already removed the object from this frontier.
    ** A black duplicate is conservatively re-traversed below; a white/dead entry
    ** is not valid mark work for this cycle and must be unlinked as stale.
    */
    lj_gc_list_pop_head_rel(&g->gc.gray, o);
    goto done;
  }
  lj_assertG(isgray(o) || black_rescan,
	     "propagation of non-gray object");
  /*
  ** A forced full collection may first finish an already-active color mark
  ** cycle before it can start a fresh major GC2 cycle. Objects left gray by a
  ** mutator publication in that older cycle still become color-live here, so
  ** mirror the object itself into GC2 at the blackening edge.
  **
  ** GC2 can also hand immutable/birth-marked objects to the color gray list as
  ** black+NEEDSCAN rescans. Those are already color-live; clear the handoff bit
  ** and traverse the payload without changing their color again.
  **
  ** A stale duplicate gray-list node can also reach the head after another entry
  ** has already blackened the same object. Re-traversing that payload is
  ** conservative and keeps resize-forwarded table edges from depending on which
  ** duplicate list node wins the race to the head.
  */
  if (LJ_UNLIKELY(gct == ~LJ_TFUNC &&
		  !gc_valid_func_obj(g, gco2func(o)))) {
    lj_gc_list_pop_head_rel(&g->gc.gray, o);
    goto done;
  }
  /*
  ** Several mutators may drive incremental GC at the same time. The gray list
  ** is lock-free, but each list node is still the object's stock intrusive
  ** gclist link, so traversal ownership is the successful head CAS.
  */
  if (!lj_gc_list_pop_head_rel(&g->gc.gray, o))
    goto done;
  /*
  ** A stale duplicate can leave a black object still linked later in the
  ** intrusive gray list. Treat a black rescan like an ordinary gray traversal
  ** while its payload is being processed, so recursive edges cannot claim and
  ** republish the same gclist link before the stale list entry is consumed.
  */
  if (black_rescan)
    black2gray(o);
  (void)lj_gc2_markobj_direct(g, o);
  /*
  ** GC2 can set NEEDSCAN before the color collector reaches this payload.  The
  ** handoff is consumed by the traversal below, regardless of whether the object
  ** arrived as an explicit black rescan or as an ordinary gray entry that GC2
  ** tagged while it was already on the color frontier.
  */
  if (lj_obj_gcflags(o) & LJ_GC_NEEDSCAN)
    gc_mark_needscan_consume(g, o);
  gray2black(o);
  if (LJ_LIKELY(gct == ~LJ_TTAB)) {
    GCtab *t = gco2tab(o);
    MSize colosz = lj_tab_colo_size(t);
    TValue *array;
    Node *node;
    MSize asize, acap, hmask;
    int tstat;
    if (lj_tab_array_snapshot_gc(g, t, &array, &asize, &acap) !=
	LJ_TAB_GC_SNAPSHOT_OK)
      acap = 0;
    if (lj_tab_node_snapshot_gc(g, t, &node, &hmask) !=
	LJ_TAB_GC_SNAPSHOT_OK)
      hmask = 0;
    UNUSED(array); UNUSED(asize); UNUSED(node);
    tstat = gc_traverse_tab(g, t);
    if (tstat == GC_TRAVERSE_TAB_TRANSIENT) {
      gc_mark_transient_requeue(g, o);
      m = GCSWEEPCOST;
      goto done;
    }
    if (tstat > 0)
      black2gray(o);  /* Keep weak tables gray. */
    m = (LJ_MAX_COLOSIZE != 0 && colosz ?
	 sizetabcolo(colosz) : sizeof(GCtab)) +
	(acap ? lj_tab_array_bytes(acap) : 0) +
	(hmask ? lj_tab_node_bytes(hmask) : 0);
  } else if (LJ_LIKELY(gct == ~LJ_TFUNC)) {
    GCfunc *fn = gco2func(o);
    gc_traverse_func(g, fn);
    m = isluafunc(fn) ? sizeLfunc((MSize)lj_funcL_nupvalues(&fn->l)) :
			sizeCfunc((MSize)lj_funcC_nupvalues(&fn->c));
  } else if (LJ_LIKELY(gct == ~LJ_TPROTO)) {
    GCproto *pt = gco2pt(o);
    gc_traverse_proto(g, pt);
    m = pt->sizept;
  } else if (LJ_LIKELY(gct == ~LJ_TTHREAD)) {
    lua_State *th = gco2th(o);
    if (gc_grayagain_thread_claim(g, th))
      lj_gc_list_push_rel(&g->gc.grayagain, o);
    black2gray(o);  /* Threads are never black. */
    gc_traverse_thread(g, th);
    m = sizeof(lua_State) + sizeof(TValue) * th->stacksize;
  } else if (LJ_UNLIKELY(gct == ~LJ_TUDATA)) {
    /*
    ** GC2-to-color rescan handoff can enqueue an already-black userdata when
    ** its side roots change during an active color cycle. Userdata are marked
    ** eagerly on the white path and are normally never gray, but the rescan node
    ** still needs to traverse metatables, environments and native root payloads.
    */
    m = gc_traverse_udata(g, gco2ud(o));
  } else if (LJ_UNLIKELY(gct == ~LJ_TUPVAL)) {
    TValue tv;
    GCupval *uv = gco2uv(o);
    lj_tv_load_acq(&tv, uvval(uv));
    gc_mark_upval_payload_tv(g, &tv);
    m = sizeof(GCupval);
  } else {
#if LJ_HASJIT
    GCtrace *T = gco2trace(o);
    IRRef nins, nk;
    gc_traverse_trace(g, T);
    nins = trace_nins_acq(T);
    nk = trace_nk_acq(T);
    m = ((sizeof(GCtrace)+7)&~7) + (nins-nk)*sizeof(IRIns) +
	 trace_nsnap_acq(T)*sizeof(SnapShot) +
	 trace_nsnapmap_acq(T)*sizeof(SnapEntry);
#else
    lj_assertG(0, "bad GC type %d", gct);
#endif
  }
done:
  gc_mark_active_dec(g);
  return m;
}

/* Propagate all gray objects. */
static size_t gc_propagate_gray(global_State *g)
{
  size_t m = 0;
  gc_test_legacy_count(g, propagate);
  while (lj_gc_list_head_acq(&g->gc.gray) != NULL)
    m += propagatemark(g);
  return m;
}

static int gc_has_traversable_payload(uint32_t gct)
{
  switch (gct) {
  case ~LJ_TFUNC:
  case ~LJ_TTAB:
  case ~LJ_TTHREAD:
  case ~LJ_TPROTO:
  case ~LJ_TUDATA:
  case ~LJ_TUPVAL:
#if LJ_HASJIT
  case ~LJ_TTRACE:
#endif
    return 1;
  default:
    return 0;
  }
}

void lj_gc_markobj_deep(global_State *g, GCobj *o)
{
  uint32_t gct;
  gc_test_legacy_count(g, markobj_deep);
  if (LJ_UNLIKELY(!gc_objroot_gct_valid(g, o, &gct)))
    return;
  /*
  ** Sweep-time VM operands are semantic roots, not stale reader bodies. FNEW
  ** may run while the previous cycle is sweeping an other-white parent closure,
  ** its upvalues, or the child prototype named by the bytecode. Body-only SMR
  ** preservation would leave those payload edges for the already-closed mark
  ** phase, so mark the operand and drain the local color frontier immediately.
  */
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
    lj_gc_markobj(g, o);
  } else if (gc_state_is_sweep(g) && gc_has_traversable_payload(gct)) {
    lj_gc_arena_markobj(g, o);
    if (iswhite(o)) {
      gc_mark(g, o);
    } else if (isblack(o)) {
      /*
      ** A forced full collection first has to finish any old sweep already in
      ** progress. Current VM roots discovered in that window are semantic roots
      ** for the old cycle, even when an earlier SMR/body-preservation path made
      ** the root header black without traversing its payload. Requeue the payload
      ** through the stock gray frontier so cyclic closure/table graphs keep the
      ** same traversal discipline as ordinary mark propagation.
      */
      gc_mark_active_inc(g);
      if (lj_gc_claim_black_to_gray(o))
	lj_gc_list_push_rel(&g->gc.gray, o);
      gc_mark_active_dec(g);
    }
  } else {
    lj_gc_arena_markobj(g, o);
    if (iswhite(o))
      gc_mark(g, o);
    else
      lj_gc_preserveobj(g, o);
  }
  (void)gc_propagate_gray(g);
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

static int gc2_free_unmarked_obj(global_State *g, GCobj *o)
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
  if (gct == (uint32_t)~LJ_TTRACE)
    return lj_trace_free_gc(g, gco2trace(o));
#endif
  if (gct >= (uint32_t)~LJ_TSTR && gct <= (uint32_t)~LJ_TUDATA) {
    GCFreeFunc fn = gc_freefunc[gct - (uint32_t)~LJ_TSTR];
    if (fn) {
      fn(g, o);
      return 1;
    }
  }
  return 0;
}

static int gc2_size_fits_mem(global_State *g, const void *p, GCSize size)
{
  TGState *tg = G2TG(g);
  GCArena *a;
  uint32_t cell, maxcells;
  if (!tg || !lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL) ||
      g->allocf != lj_arena_allocf)
    return 1;
  if (!p || size > LJ_MAX_MEM32)
    return 0;
  a = lj_arena_of(p);
  if (lj_arena_ishuge(a))
    return size <= LJ_MAX_MEM32;
  cell = lj_arena_cellof(p);
  if (cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS)
    return 0;
  maxcells = LJ_ARENA_CELLS - cell;
  return lj_arena_ncells(size) <= maxcells;
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
      !lj_gc2_obj_valid(g, obj2gco(fn)) || fn->c.gct != ~LJ_TFUNC)
    return 0;
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
  minpt = (MSize)sizeof(GCproto) + pt->sizebc*(MSize)sizeof(BCIns);
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

static int gc2_valid_tab_obj(global_State *g, GCtab *t)
{
  int8_t colo = lj_tab_colo_acq(t);
  MSize colosz = lj_tab_colo_size(t);
  MSize asize, acap;
  TValue *array;
  TValue *coloarray = (TValue *)(void *)((char *)(void *)t + sizeof(GCtab));
  Node *node;
  MSize hmask;
  GCSize bodysize;

  if (lj_tab_array_snapshot_gc(g, t, &array, &asize, &acap) !=
      LJ_TAB_GC_SNAPSHOT_OK ||
      lj_tab_node_snapshot_gc(g, t, &node, &hmask) !=
      LJ_TAB_GC_SNAPSHOT_OK)
    return 0;
  if (LJ_MAX_COLOSIZE != 0 && colosz > LJ_MAX_COLOSIZE)
    return 0;
  if (asize > LJ_MAX_ASIZE || acap > LJ_MAX_ASIZE)
    return 0;
  if (colo > 0) {
    if (array != coloarray || asize > colosz || lj_tab_acap_acq(t) > colosz)
      return 0;
  } else if (array == coloarray) {
    /*
    ** A negative colocated marker means a resize has split the old inline array
    ** from table indexing. A dead table still pointing at the inline storage is
    ** in a transient resize state; ordinary sweep must not free it as separated.
    */
    return 0;
  } else if (array != NULL) {
    if (!gc2_size_fits_mem(g, lj_tab_array_hdrw(array),
			   lj_tab_array_bytes(acap)))
      return 0;
  }

  if (node == NULL)
    return 0;
  if (!gc2_valid_pow2_mask(hmask))
    return 0;
  if (hmask > 0) {
    if (!gc2_size_fits_mem(g, lj_tab_node_hdrw(node),
			   lj_tab_node_bytes(hmask)))
      return 0;
  }

  bodysize = (LJ_MAX_COLOSIZE != 0 && colosz) ?
	     (GCSize)sizetabcolo(colosz) : (GCSize)sizeof(GCtab);
  return gc2_size_fits_arena(g, obj2gco(t), bodysize);
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

static int gc2_valid_freeable_obj(global_State *g, GCobj *o)
{
  uint32_t gct = o->gch.gct;
  if (gct == (uint32_t)~LJ_TSTR)
    return 0;  /* String table sweep owns GCstr lifetime. */
  if (gc2_cdata_finalizer_pending(o))
    return 0;  /* FINREG dispatch must clear LJ_GC_CDATA_FIN before free. */
#if LJ_HASFFI
  if (gct == (uint32_t)~LJ_TCDATA &&
      !lj_cdata_validate(g, gco2cd(o), NULL, NULL))
    return 0;  /* Stale cdata header: ctype/size is not safe to dispatch. */
#endif
  if (gct == (uint32_t)~LJ_TPROTO && !gc2_valid_proto_obj(g, gco2pt(o)))
    return 0;  /* Stale proto header: sizept is not safe for destructor. */
  if (gct == (uint32_t)~LJ_TFUNC && !gc2_valid_func_free_obj(g, gco2func(o)))
    return 0;  /* Free only needs a sane closure body; traversal stays strict. */
  if (gct == (uint32_t)~LJ_TTHREAD &&
      mref(gco2th(o)->glref, global_State) != g)
    return 0;  /* Stale thread header: glref is not safe for destructor. */
  if (gct == (uint32_t)~LJ_TTAB && !gc2_valid_tab_obj(g, gco2tab(o)))
    return 0;  /* Stale table header: side-vector sizes are not trustworthy. */
  if (gct == (uint32_t)~LJ_TUDATA) {
    GCSize size;
    if (!gc_udata_payload_valid(gco2ud(o), &size) ||
	!gc2_size_fits_arena(g, o, size))
      return 0;  /* Stale userdata header: payload size is not trustworthy. */
  }
  return gct >= (uint32_t)~LJ_TSTR && gct <= (uint32_t)~LJ_TUDATA &&
	 gc_freefunc[gct - (uint32_t)~LJ_TSTR] != NULL;
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

void lj_gc_unlink_root_obj(global_State *g, GCobj *dead)
{
  GCRef *p = lj_gc_root_ref(g);
  GCobj *o;
  if (!g || !dead)
    return;
  (void)lj_gc_flush_root_pending(g);
  (void)lj_gc_repair_root_spine(g);
  while ((o = gcref_acq(*p)) != NULL) {
    if (o == dead) {
      if (gc_chain_splice(p, o))
	return;
      gc_root_wait_no_l();
      continue;
    }
    p = lj_obj_gcwref(o);
  }
}

static int gc_root_chain_contains_obj(global_State *g, GCobj *needle)
{
  GCobj *o;
  uint32_t n = 0;
  if (!g || !needle)
    return 0;
  for (o = lj_gc_root_acq(g); o != NULL && n++ < LJ_GC2_ROOT_SCAN_LIMIT;) {
    GCobj *next;
    if (LJ_UNLIKELY(!gc_root_link_valid(g, o)))
      break;
    if (o == needle)
      return 1;
    next = lj_obj_gcw_acq(o);
    if (next == o)
      break;
    o = next;
  }
  return 0;
}

#define GC2_ROOT_PRUNE_BATCH 256u

static void *gc2_sweep_obj_base(global_State *g, GCobj *o)
{
#if LJ_HASFFI
  if (o && o->gch.gct == (uint32_t)~LJ_TCDATA) {
    GCcdata *cd = gco2cd(o);
    void *base;
    if (cdataisv(cd) && lj_cdata_validate(g, cd, &base, NULL))
      return base;
  }
#else
  UNUSED(g);
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

/* Classify one exact small old-generation allocation after its ownership-spine
** link has been detached. LIVE means it must be reanchored after the grace;
** RETIRED means its destructor is pending. WHITE remains reserved for raw or
** fixed allocations which were never detached. */
static void gc2_sweep_detached_small(global_State *g, GCArena *a,
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
}

static void gc2_sweep_detached_obj(global_State *g, GCobj *o, int marked)
{
  void *base = gc2_sweep_obj_base(g, o);
  GCArena *a = lj_arena_of(base);
  if (!lj_arena_ishuge(a)) {
    uint32_t cell = lj_arena_cellof(base);
    if (cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS)
      gc2_sweep_detached_small(g, a, cell, marked);
    return;
  }
  {
    uint32_t owner_tid = lj_arena_owner_acq(a);
    TGState *tg = lj_tg_find_owner(g, owner_tid);
    int ticketed;
    if (!tg || !lj_tg_flags_test_acq(tg, TGF_HUGETAB))
      return;
    /* One huge mapping contains one allocation. Publish an explicit metadata
    ** ticket for both dead and already-marked detached roots: RETIRED is the
    ** destructor claim, while MARK|TICKET is the post-grace reanchor claim.
    ** A concurrent marker may win either side of this call without making the
    ** exact variable-offset header undiscoverable. */
    ticketed = lj_arena_hugetab_retire(&tg->huge, base, o,
					      lj_gc2_retire_epoch(g), NULL);
    lj_assertG(ticketed, "detached huge root lost ownership ticket");
    gc2_sweep_grace_needed_rel(g, 1);
    if (!ticketed)
      return;  /* Retain the mapped body; never invent a destructor owner. */
    UNUSED(marked);
  }
}

uint32_t lj_gc_sweep_gc2_unmarked(global_State *g)
{
  GCRef *p;
  GCobj *o;
  uint32_t seen = 0, unlinked = 0;
  if (!g || gc2_phase_acq(g) != LJ_GC2_SWEEP ||
      gc2_sweep_root_done_acq(g))
    return 0;
  p = gc2_sweep_root_cursor_acq(g);
  if (!p)
    p = lj_gc_root_ref(g);
  while (seen < GC2_ROOT_PRUNE_BATCH && (o = gcref_acq(*p)) != NULL) {
    int marked;
    seen++;
    if (LJ_UNLIKELY(!gc_root_link_valid(g, o))) {
      /* Quarantined bodies remain mapped, but an unregistered pointer is not a
      ** cursor-safe ownership entry. Restart after spine repair in a later
      ** bounded batch instead of dereferencing an unknown successor. */
      (void)lj_gc_repair_root_spine(g);
      p = lj_gc_root_ref(g);
      break;
    }
    gc2_preserve_root_spine_body(g, o);
    if (!gc2_sweep_obj_old_generation(g, o)) {
      p = lj_obj_gcwref(o);  /* Post-reset allocation: never this sweep. */
      continue;
    }
    marked = lj_gc2_ismarked(g, o);
    if (LJ_UNLIKELY(gc2_cdata_finalizer_pending(o)) ||
	(lj_obj_gcflags(o) & (LJ_GC_FIXED|LJ_GC_SFIXED))) {
      (void)lj_gc2_markmem(g, gc2_sweep_obj_base(g, o));
      p = lj_obj_gcwref(o);
      continue;
    }
    if (o->gch.gct == 0 || gc2_valid_freeable_obj(g, o)) {
#if LJ_HASJIT
      if (marked == 0 && o->gch.gct == (uint32_t)~LJ_TTRACE &&
	  !lj_trace_retire_gc_claim(g, gco2trace(o))) {
	/* The nonwaiting recorder-token attempt left trace/root/state unchanged. */
	break;
      }
#endif
      if (!gc_chain_splice(p, o))
	continue;  /* Concurrent insertion at this link; revisit boundedly. */
      gc2_sweep_detached_obj(g, o, marked > 0);
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

static GCobj *gc2_sweep_cell_obj(global_State *g, GCArena *a,
				  uint32_t cell, uint32_t end)
{
  GCobj *o = (GCobj *)lj_arena_cellptr(a, cell);
#if LJ_HASFFI
  {
    char *base = (char *)o;
    size_t bytes = (size_t)(end - cell) << LJ_CELL_SHIFT;
    uint16_t offset = la_load16_acq((uint16_t *)(void *)base);
    if (offset >= sizeof(GCcdataVar) &&
	(size_t)offset + sizeof(GCcdata) <= bytes) {
      GCcdata *cd = (GCcdata *)(void *)(base + offset);
      void *realbase = NULL;
      GCSize size = 0;
      if (cd->gct == ~LJ_TCDATA && cdataisv(cd) &&
	  memcdatav(cd) == (void *)base &&
	  lj_cdata_validate(g, cd, &realbase, &size) &&
	  realbase == (void *)base && size <= bytes)
	return obj2gco(cd);
    }
  }
#else
  UNUSED(end);
#endif
  if (o->gch.gct == 0 || gc2_valid_freeable_obj(g, o))
    return o;
  return NULL;
}

static uint32_t gc2_sweep_arena_bodies(global_State *g, GCArena *a,
				       int unmarked_only)
{
  uint32_t i, n = 0;
  GCobj *o;
  if (!g || !a)
    return 0;
  if (unmarked_only) {
    (void)lj_gc_flush_root_pending(g);
    (void)lj_gc_repair_root_spine(g);
  }
  for (i = LJ_AFIRST_CELL; i < LJ_ARENA_CELLS; i++) {
    uint32_t state;
    if (!lj_arena_bm_get(a->block, i))
      continue;
    if (!unmarked_only) {
      o = (GCobj *)lj_arena_cellptr(a, i);
      if (isdead(g, o) && gc2_valid_freeable_obj(g, o)) {
	lj_gc_unlink_root_obj(g, o);
	if (gc2_free_unmarked_obj(g, o))
	  n++;
      }
      continue;
    }
    state = lj_arena_sweep_state_acq(a, i);
    if (state != LJ_ARENA_SWEEP_WHITE || lj_arena_bm_get(a->mark, i))
      continue;
    /* Every GC object is ownership-spine linked before publication. The
    ** bounded bridge classified all old nonfixed headers before arena scan, so
    ** a remaining WHITE allocation is raw/opaque storage. Retain it unless its
    ** owning subsystem physically frees it (which transitions FREEING). Never
    ** infer a destructor from attacker-controlled payload bytes. */
    (void)la_bit_test_and_set64(&a->mark[i >> 6], i & 63);
  }
  return n;
}

uint32_t lj_gc_sweep_gc2_arena_unmarked(global_State *g, GCArena *a)
{
  return gc2_sweep_arena_bodies(g, a, 1);
}

uint32_t lj_gc_reclaim_gc2_arena(global_State *g, GCArena *a,
				  uint32_t limit, int *donep)
{
  uint32_t cell, scanned = 0, changed = 0;
  int pending = 0;
  if (donep)
    *donep = 0;
  if (!g || !a || limit == 0 ||
      !(lj_arena_flags_acq(a) & LJ_AF_QUARANTINE))
    return 0;
  cell = a->hdr.reclaim_cell;
  if (cell < LJ_AFIRST_CELL || cell > LJ_ARENA_CELLS)
    cell = LJ_AFIRST_CELL;
  while (cell < LJ_ARENA_CELLS && scanned < limit) {
    uint32_t state;
    if (!lj_arena_bm_get(a->block, cell)) {
      cell++;
      scanned++;
      continue;
    }
    state = lj_arena_sweep_state_acq(a, cell);
    if (state == LJ_ARENA_SWEEP_LIVE ||
	state == LJ_ARENA_SWEEP_RETIRED) {
      uint32_t end = gc2_sweep_alloc_end(a, cell);
      GCobj *o = gc2_sweep_cell_obj(g, a, cell, end);
      if (!o) {
	/* LIVE/RETIRED is published only from an exact ownership-spine header.
	** Refuse bitmap publication if that proof cannot be reconstructed. */
	lj_assertG(0, "cannot reconstruct detached arena GC header");
	pending = 1;
	cell++;
	scanned++;
	continue;
      }
      if (state == LJ_ARENA_SWEEP_LIVE) {
	if (o->gch.gct == 0) {
	  if (lj_arena_sweep_state_cas(a, cell, LJ_ARENA_SWEEP_LIVE,
					      LJ_ARENA_SWEEP_FREEING))
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
	  pending = 1;
	  cell++;
	  scanned++;
	  continue;
	}
#endif
	/* The detach left gcw untouched for pre-grace root readers. Now no such
	** reader remains, publish one fresh ownership link and retire LIVE state. */
	lj_gc_linkobj(g, o);
	if (lj_arena_sweep_state_cas(a, cell, LJ_ARENA_SWEEP_LIVE,
					    LJ_ARENA_SWEEP_WHITE))
	  changed++;
	else
	  pending = 1;  /* A concurrent physical free won; revisit conservatively. */
      } else {
        if (o->gch.gct == 0) {
	  if (lj_arena_sweep_state_cas(a, cell, LJ_ARENA_SWEEP_RETIRED,
					      LJ_ARENA_SWEEP_FREEING)) {
	    uint32_t old = lj_arena_reclaim_deferred_sub(a, 1);
	    lj_assertG(old != 0, "destroyed-body deferred underflow");
	    UNUSED(old);
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
	  pending = 1;
	} else
#endif
	if (lj_arena_sweep_state_cas(a, cell, LJ_ARENA_SWEEP_RETIRED,
					    LJ_ARENA_SWEEP_FREEING)) {
	  uint32_t old = lj_arena_reclaim_deferred_sub(a, 1);
	  int freed;
	  lj_assertG(old != 0, "arena destructor deferred underflow");
	  UNUSED(old);
	  freed = gc2_free_unmarked_obj(g, o);
	  if (LJ_UNLIKELY(!freed)) {
	    (void)lj_arena_reclaim_deferred_add(a, 1);
	    (void)lj_arena_sweep_state_cas(a, cell,
	      LJ_ARENA_SWEEP_FREEING, LJ_ARENA_SWEEP_RETIRED);
	    pending = 1;
	  } else {
	    changed++;
	  }
	} else {
	  pending = 1;  /* RETIRED->LIVE rescue or physical-free completion. */
	}
      }
    }
    cell++;
    scanned++;
  }
  a->hdr.reclaim_cell = cell;
  if (cell == LJ_ARENA_CELLS) {
    uint32_t deferred = lj_arena_reclaim_deferred_acq(a);
    if (deferred == 0 && !pending) {
      if (donep)
	*donep = 1;
    } else {
      a->hdr.reclaim_cell = LJ_AFIRST_CELL;
    }
  }
  /* Cursor advancement is bounded real work and must keep the owner scheduled
  ** until it reaches EOF. At EOF, a token-busy trace returns zero so the worker
  ** can park rather than spin; JIT token release wakes it for another pass. */
  if (changed)
    return changed;
  return cell < LJ_ARENA_CELLS && scanned != 0 ? 1u : 0u;
}

uint32_t lj_gc_reclaim_gc2_huge(global_State *g, TGState *tg, void *p,
				 const LJHugeInfo *hi, int *pendingp)
{
  GCArena *a;
  GCobj *o;
  uint32_t flags;
  if (pendingp)
    *pendingp = 0;
  if (!g || !tg || !p || !hi)
    return 0;
  a = lj_arena_of(p);
  flags = hi->flags;
  if (flags & LJ_HUGEF_BUSY) {
    if (pendingp) *pendingp = 1;
    return 0;  /* Publisher/reanchor owns all mapping-header access. */
  }
  o = (flags & LJ_HUGEF_TICKET) ?
      (GCobj *)la_loadptr_acq((void *const *)&a->hdr.retire_obj) : NULL;

  if (flags & LJ_HUGEF_FREEING) {
    uint64_t retire_epoch = la_load64_acq(&a->hdr.retire_epoch);
    uint64_t now = lj_gc2_retire_epoch(g);
    if (retire_epoch == ~(uint64_t)0) {
      la_store64_rel(&a->hdr.retire_epoch, now);
      gc2_sweep_grace_needed_rel(g, 1);
      if (pendingp) *pendingp = 1;
      return 1;
    }
    if (retire_epoch >= now) {
      if (pendingp) *pendingp = 1;
      return 0;
    }
  }

  if (flags & LJ_HUGEF_MARK) {
    if (o && (flags & LJ_HUGEF_TICKET)) {
#if LJ_HASJIT
      if (o->gch.gct == (uint32_t)~LJ_TTRACE &&
	  la_load64_acq(&gco2trace(o)->retire_epoch) != 0) {
	(void)lj_trace_free_gc(g, gco2trace(o));
	if (pendingp) *pendingp = 1;
	return 0;
      }
#endif
      if (!gc2_valid_freeable_obj(g, o)) {
	if (pendingp) *pendingp = 1;
	return 0;
      }
      if (!lj_arena_hugetab_claim_live_ticket(&tg->huge, p, NULL)) {
	if (pendingp) *pendingp = 1;
	return 0;
      }
      /* Every old nonfixed root was detached before the grace. One mapping has
      ** one allocation, so retire_obj is an exact single reanchor ticket. */
      lj_gc_linkobj(g, o);
      la_storeptr_rel(&a->hdr.retire_obj, NULL);
      {
	int finished = lj_arena_hugetab_finish_live_ticket(&tg->huge, p, NULL);
	lj_assertG(finished, "huge live ticket lost after root reanchor");
	if (!finished) {
	  if (pendingp) *pendingp = 1;
	  return 0;
	}
      }
      return 1;
    }
    return 0;  /* Marked raw/fixed huge storage was never detached. */
  }

  if (!(flags & (LJ_HUGEF_RETIRED|LJ_HUGEF_FREEING))) {
    if (o && lj_arena_hugetab_retire(&tg->huge, p, o,
					     lj_gc2_retire_epoch(g), NULL)) {
      gc2_sweep_grace_needed_rel(g, 1);
      if (pendingp) *pendingp = 1;
      return 1;
    }
    /* Opaque huge storage has no GC destructor proof. Preserve it rather than
    ** interpreting payload bytes as a header. */
    (void)lj_arena_hugetab_mark(&tg->huge, p, NULL);
    return 1;
  }

  if (!o) {
    /* An external subsystem owns the logical free. FREEING is enough to let
    ** this sole huge-table owner perform the final delete after the grace. */
    if (!(flags & LJ_HUGEF_FREEING) &&
	!lj_arena_hugetab_claim_freeing(&tg->huge, p, NULL)) {
      if (pendingp) *pendingp = 1;
      return 0;
    }
  } else {
#if LJ_HASJIT
    if (o->gch.gct == (uint32_t)~LJ_TTRACE &&
	!lj_trace_body_destroyed_acq(gco2trace(o))) {
      (void)lj_trace_free_gc(g, gco2trace(o));
      if (pendingp) *pendingp = 1;
      return 0;
    }
#endif
    if (!(flags & LJ_HUGEF_FREEING)) {
      if (!lj_arena_hugetab_claim_freeing(&tg->huge, p, NULL)) {
	if (pendingp) *pendingp = 1;
	return 0;
      }
      if (o->gch.gct != 0 && !gc2_free_unmarked_obj(g, o)) {
	(void)lj_arena_hugetab_revert_retired(&tg->huge, p);
	if (pendingp) *pendingp = 1;
	return 0;
      }
    }
  }
  {
    LJHugeInfo snap;
    if (lj_arena_hugetab_delete(&tg->huge, p, &snap) == 1) {
      /* No access to a/a->hdr is legal after this unmap. */
      lj_arena_huge_unmap(p, snap.size);
      return 1;
    }
  }
  return 0;
}

uint32_t lj_gc_sweep_gc2_all_arena_bodies(global_State *g)
{
  TGState *tg;
  uint32_t total = 0;
  if (!g)
    return 0;
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg)) {
    GCArena *a;
    if (!lj_gc2_sweep_tg_ready(tg))
      continue;
    for (a = tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE]; a;
	 a = lj_arena_next_acq(a))
      total += gc2_sweep_arena_bodies(g, a, 0);
    for (a = tg->alloc.owned[LJ_ARENAK_TRAVERSABLE]; a;
	 a = lj_arena_next_acq(a))
      total += gc2_sweep_arena_bodies(g, a, 0);
  }
  return total;
}

/* Check whether we can clear a key or a value slot from a table. */
static int gc_mayclear(global_State *g, cTValue *o, int val)
{
  /*
  ** Weak clearing reads slots without owning their table generation. A stale
  ** tagged GC pointer whose header has been reused is not a live weak edge.
  ** Clear it instead of dereferencing reclaimed header state.
  */
  if (LJ_UNLIKELY(!gc_tv_gcref_type_match(g, o)))
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
  while (o) {
    GCtab *t = gco2tab(o);
    lj_assertG((lj_obj_gcflags(obj2gco(t)) & LJ_GC_WEAK),
	       "clear of non-weak table");
    if ((lj_obj_gcflags(obj2gco(t)) & LJ_GC_WEAKVAL)) {
      TValue *array;
      MSize i, asize, acap;
      if (lj_tab_array_snapshot_gc(g, t, &array, &asize, &acap) ==
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
	    (void)lj_tab_clear_weak_slot_keyed(t, slot, &key, &val);
	  }
	}
      }
    }
    {
      MSize i, hmask;
      Node *node;
      if (lj_tab_node_snapshot_gc(g, t, &node, &hmask) ==
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
	      (void)lj_tab_clear_weak_slot_keyed(t, slot, &key, &val);
	  }
	}
      }
    }
    o = lj_tab_gclist_acq(t);
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

static void gc2_shutdown_free_obj(global_State *g, GCobj *o)
{
  uint32_t gct;
  int deferred;
  if (!gc2_valid_freeable_obj(g, o)) {
    /* A stale ownership-spine entry has either already had its destructor run
    ** or belongs to terminal arena teardown. Never dispatch from an untrusted
    ** type byte merely because the process is closing. */
    return;
  }
  gct = o->gch.gct;
  deferred = gc2_deferred_body_pending(g, o);
#if LJ_HASJIT
  if (gct == (uint32_t)~LJ_TTRACE) {
    (void)lj_trace_free_gc(g, gco2trace(o));
    return;  /* Trace retirement owns its intact header until the SMR drain. */
  }
#endif
  gc_freefunc[gct - (uint32_t)~LJ_TSTR](g, o);
  if (deferred)
    la_store8_rel(&o->gch.gct, 0);  /* Arena body awaits terminal unmap. */
}

static uint32_t gc2_shutdown_free_roots(global_State *g,
					 GCShutdownSeen *seen)
{
  GCobj *maino = obj2gco(mainthread_acq(g));
  GCRef *p = lj_gc_root_ref(g);
  GCobj *o;
  uint32_t freed = 0;
  gc_shutdown_seen_clear(seen);
  while ((o = gcref_acq(*p)) != NULL) {
    GCobj *next;
    int seenrc = gc_shutdown_seen_add(seen, o);
    if (seenrc <= 0) {
      /* A duplicate intrusive node necessarily closes a cycle: one object has
      ** only one gcw link. Allocation failure is also fail-safe: stop before
      ** dereferencing an untracked body rather than risking terminal UAF. */
      setgcrefnullrel(*p);
      break;
    }
    if (!gc_root_link_valid(g, o)) {
      setgcrefnullrel(*p);
      break;
    }
    next = lj_obj_gcw_acq(o);
    if (o == maino) {
      p = lj_obj_gcwref(o);
      continue;
    }
    if (next)
      setgcrefrel(*p, next);
    else
      setgcrefnullrel(*p);
    gc2_shutdown_free_obj(g, o);
    freed++;
  }
  return freed;
}

static void gc2_shutdown_free_strings(global_State *g,
				      GCShutdownSeen *seen)
{
  StrTabHdr *hdr = lj_str_tabh_acq(g);
  MSize i;
  if (!hdr)
    return;
  gc_shutdown_seen_clear(seen);
  for (i = hdr->mask; i != ~(MSize)0; i--) {
    GCRef *bucket = &hdr->bucket[i];
    uintptr_t u = lj_str_ref_load_acq(bucket);
    GCobj *o = (GCobj *)(u & ~(uintptr_t)LJ_STRHASH_LINKMASK);
    lj_str_ref_store_rel(bucket, u & LJ_STRHASH_SECONDARY);
    while (o) {
      GCobj *next;
      int seenrc = gc_shutdown_seen_add(seen, o);
      if (seenrc <= 0)
	break;
      next = lj_str_next_acq(o);
      if (la_load8_acq(&o->gch.gct) == (uint8_t)~LJ_TSTR)
	lj_str_free(g, gco2str(o));
      o = next;
    }
  }
}

/* Free all remaining GC objects with GC2 ownership metadata only. This is a
** terminal, single-threaded drain after threading shutdown and finalizers; it
** deliberately never starts the retired color marker or sweeper. */
void lj_gc2_freeall(global_State *g)
{
  GCShutdownSeen seen;
  GCobj *maino;
  uint32_t rounds = 0;
  memset(&seen, 0, sizeof(seen));
  (void)lj_gc_repair_root_spine(g);
  do {
    (void)lj_gc_flush_root_pending(g);
    if (gc2_shutdown_free_roots(g, &seen) == 0)
      break;
  } while (++rounds < 16u);  /* Closing a thread can publish closed upvalues. */
  (void)lj_gc_flush_root_pending(g);
  (void)gc2_shutdown_free_roots(g, &seen);
  maino = obj2gco(mainthread_acq(g));
  lj_obj_setgcwrel(maino, NULL);
  lj_gc_root_rel(g, maino);
  gc2_shutdown_free_strings(g, &seen);
  gc_shutdown_seen_fini(&seen);
}

#if LJ_HASJIT
int lj_gc2_jit_needs_exit(global_State *g)
{
  uint32_t phase = gc2_phase_acq(g);
  return lj_tg_any_jit_active(g) &&
	 (phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK ||
	  phase == LJ_GC2_SWEEP ||
	  lj_gc_total_load(g) >= lj_gc_threshold_load(g) ||
	  gc_hard_assist_due_jit(g));
}
#else
#define lj_gc2_jit_needs_exit(g)	0
#endif

static int gc2_step_auto(lua_State *L, int threshold_step, uint32_t step_limit)
{
  global_State *g = G(L);
  GCSize quantum = gc_step_debt_quantum(g);
  int32_t ostate = vmstate_load_acq(g);
  int running = gc_logical_running(g);
  int drove = 0;
  int done = 0;
  if (step_limit == 0)
    step_limit = 1;
  setvmstate(g, GC);
  if (running)
    lj_gc2_check_trigger(g, L2TG(L));
  while (running && step_limit-- != 0) {
    uint32_t phase = gc2_phase_acq(g);
    if (phase == LJ_GC2_IDLE && gc2_cycle_leader_acq(g) == 0 &&
	!threshold_step)
      break;
    drove = 1;
    done = lj_gc2_step_explicit(L, 1);
    if (gc2_phase_acq(g) == LJ_GC2_IDLE)
      break;
  }
  g->gc.debt = 0;
  if (gc2_phase_acq(g) == LJ_GC2_IDLE) {
    if (running)
      lj_gc2_publish_idle_threshold(g);
    vmstate_store_rel(g, ostate);
    return done ? 1 : (drove ? 0 : -1);
  }
  lj_gc_threshold_store(g, lj_gc_total_load(g) + quantum);
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
  if (threshold_step)
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
  if (lj_gc2_jit_needs_exit(g))
    return 1;
  lj_gc2_check_trigger(g, tg);
  threshold_step = lj_gc_total_load(g) >= lj_gc_threshold_load(g);
  hard_step = gc_hard_assist_due_jit(g);
  if (hard_step) {
    gc2_jit_hard_checks_add(g, 1);
    lj_gc2_assist(g, tg);  /* 05 section 5.11 trace-side assist bridge. */
    lj_gc2_hard_check_advance(g, lj_gc2_alloc_since_load(g));
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
  lj_tv_load_acq(&snap, tv);
  if (tvisgcv(&snap)) {
    if (!lj_gc2_tv_gcref_valid_edge(g, &snap))
      return;
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
  uint32_t gct;
  TValue tv;
  if (!L)
    return;
  g = G(L);
  if (LJ_UNLIKELY(!gc_objroot_gct_valid(g, o, &gct)))
    return;
  setgcV(L, &tv, o, ~gct);
  lj_gc_pubroot(L, &tv);
}

/* Compatibility entry for a published object edge. GC2 owns the runtime
** frontier; the legacy color frontier is never entered. */
void lj_gc_barrierf(global_State *g, GCobj *o, GCobj *v)
{
  UNUSED(o);
  if (!g || !v)
    return;
  if (gc2_phase_acq(g) == LJ_GC2_SWEEP)
    (void)lj_gc2_trace_sweep_root(g, v);
  else if (gc2_phase_acq(g) != LJ_GC2_IDLE)
    (void)lj_gc2_markobj_direct(g, v);
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

/* Publication wrapper for x64 VM table -> object stores. */
void lj_gc_pubtabobj_vm(lua_State *L, GCtab *t, GCobj *o)
{
  global_State *g;
  uint32_t gct;
  if (!L || !t || !o)
    return;
  g = G(L);
  if (LJ_UNLIKELY(!gc_objroot_gct_valid(g, obj2gco(t), &gct) ||
		  gct != (uint32_t)~LJ_TTAB ||
		  !gc_objroot_gct_valid(g, o, NULL)))
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
  global_State *g;
  uint32_t gct;
  if (!L || !t || !tv || n == 0)
    return;
  g = G(L);
  if (LJ_UNLIKELY(!gc_objroot_gct_valid(g, obj2gco(t), &gct) ||
		  gct != (uint32_t)~LJ_TTAB))
    return;
  lj_gc2_barrier_tvn_pair_g(g, obj2gco(t), tv, n);
  lj_gc2_barrier_tab(L, t);  /* Preserve the previous TSETM table barrier. */
}

/* Publication wrapper for x64 VM table -> stack loads. */
void lj_gc_pubtvroot_vm(lua_State *L, cTValue *tv)
{
  if (!L || !tv)
    return;
  lj_state_stack_pubtv(L, L, tv);
}

/* Publication wrapper for closed-upvalue TValue stores. Pass &uv->tv. */
void LJ_FASTCALL lj_gc_pubuv(global_State *g, TValue *tv)
{
  GCupval *uv = (GCupval *)((char *)tv - offsetof(GCupval, tv));
  TValue snap;
  lj_tv_load_acq(&snap, tv);
  if (LJ_UNLIKELY(!lj_gc2_tv_gcref_valid_edge(g, &snap)) ||
      !tvisgcv(&snap))
    return;
  lj_gc2_barrier_tv_pair_g(g, obj2gco(uv), &snap);
}

/* Close upvalue. Also needs a write barrier. */
void lj_gc_closeuv(global_State *g, GCupval *uv)
{
  GCobj *o = obj2gco(uv);
  /* Copy stack slot to upvalue itself and point to the copy. */
  copyTVrel(mainthread_acq(g), &uv->tv, uvval(uv));
  setmref(uv->v, &uv->tv);
  uv->closed = 1;
  /*
  ** lj_func_closeuv() has already removed the upvalue from the thread-open
  ** chain and g->uvhead, so its nextgc link is available for object-list
  ** publication. Liveness is still discovered through closures that reference
  ** the GCupval; the object list is the sweep/free spine and every consumer of
  ** that spine flushes pending roots first. Queueing here avoids a global
  ** root-list CAS on the close-upvalue path without changing collector reach.
  */
  lj_gc_linkobj_pending(g, o);
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
    TGState *tg = lj_tg_find_owner(g, owner_tid);
    if (tg)
      return gc_arena_allocd_for_tg(g, tg);
  }
  return (LJArenaAllocD *)g->allocd;
}

/* Allocate a new fragment without raising an error on allocation failure. */
void *lj_mem_new_nothrow(lua_State *L, GCSize size)
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
  lj_gc2_account_alloc(g, L2TG(L), size);  /* 04 section 4.8. */
  return p;
}

/* Call pluggable memory allocator to allocate or resize a fragment. */
void *lj_mem_realloc(lua_State *L, void *p, GCSize osz, GCSize nsz)
{
  global_State *g = G(L);
  lj_assertG((osz == 0) == (p == NULL), "realloc API violation");
  if (g->allocf == lj_arena_allocf) {
    LJArenaAllocD *ad = p ? gc_arena_allocd_for_ptr(g, p) :
			    gc_arena_allocd_for_new(L);
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

/* Allocate raw storage for a GC object without linking or throwing. */
void *lj_mem_newgco_raw_nothrow(lua_State *L, GCSize size, uint32_t flags)
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
  lj_gc2_account_alloc(g, L2TG(L), size);  /* 04 section 4.8. */
  return o;
}

/* Allocate raw storage for a GC object without linking it. */
void *lj_mem_newgco_raw(lua_State *L, GCSize size, uint32_t flags)
{
  void *o = lj_mem_newgco_raw_nothrow(L, size, flags);
  if (o == NULL)
    lj_err_mem(L);
  return o;
}

void *lj_mem_newgco_unlinked_nothrow(lua_State *L, GCSize size)
{
  return lj_mem_newgco_raw_nothrow(L, size, LJ_AF_TRAVERSABLE);
}

void *lj_mem_newgco_unlinked(lua_State *L, GCSize size)
{
  return lj_mem_newgco_raw(L, size, LJ_AF_TRAVERSABLE);
}

void lj_gc_linkobj(global_State *g, GCobj *o)
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
  lj_gcroot_repair_epoch_add(g);
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
  GCobj *tail, *next;
  uint32_t n = 0;
  if (!head) {
    *tailp = NULL;
    return 0;
  }
  (void)gc_root_chain_break_cycle(g, head);
  tail = head;
  do {
    if (!gc_root_link_valid(g, tail))
      break;
    if (n != ~(uint32_t)0)
      n++;
    next = lj_obj_gcw_acq(tail);
    if (!next)
      break;
    if (!gc_root_link_valid(g, next))
      break;
    tail = next;
  } while (1);
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
    if (!gc_root_link_valid(g, o))
      return 0;
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
  if (!anchor || !n)
    return 0;
  gc_root_prepend_chain_at(g, lj_obj_gcwref(anchor), head, tail);
  if (LJ_UNLIKELY(!gc_root_chain_contains_obj(g, anchor)))
    n += gc_root_prepend_chain(g, anchor);
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
  if (!g)
    return 0;
  main_tg = g->main_tg;
  self = lj_thr_get_tg();
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
  return n;
}

static void gc_linkobj_pending(global_State *g, GCobj *o)
{
  TGState *tg = lj_thr_get_tg();
  GCobj *head;
  if (!tg || tg->gl != g || lj_tg_flags_test_acq(tg, TGF_DEAD)) {
    lj_gc_linkobj(g, o);
    return;
  }
  if (LJ_LIKELY(tg == g->main_tg && mt_active_acq(g) == 0 &&
		mt_entering_acq(g) == 0 && gc2_n_workers_acq(g) == 0)) {
    /*
    ** Before secondary Lua threads, entering secondary attachers, or GC
    ** workers exist, the main TG is the only pending-root producer and
    ** flusher. Avoid the CAS/RMW allocation tax, but keep release publication
    ** so a later activation sees a complete pending chain.
    */
    head = lj_tg_gcroot_pending_acq(tg);
    gc_root_set_next_rel(o, head);
    lj_tg_gcroot_pending_store_transition_rel(tg, head, o);
    return;
  }
  head = lj_tg_gcroot_pending_acq(tg);
  do {
    gc_root_set_next_rel(o, head);
  } while (!lj_tg_gcroot_pending_cas(tg, &head, o));
}

void lj_gc_linkobj_pending(global_State *g, GCobj *o)
{
  gc_linkobj_pending(g, o);
}

void lj_gc_linkobj_new(global_State *g, GCobj *o)
{
  gc_linkobj_pending(g, o);
}

void lj_gc_linkobj_new_chain(global_State *g, GCobj *head, GCobj *tail)
{
  TGState *tg;
  GCobj *oldhead;
  if (!head || !tail)
    return;
  tg = lj_thr_get_tg();
  if (!tg || tg->gl != g || lj_tg_flags_test_acq(tg, TGF_DEAD)) {
    gc_root_prepend_known_chain(g, head, tail);
    return;
  }
  if (LJ_LIKELY(tg == g->main_tg && mt_active_acq(g) == 0 &&
		mt_entering_acq(g) == 0 && gc2_n_workers_acq(g) == 0)) {
    /*
    ** Publish a freshly initialized object run with one release store. The
    ** caller owns head..tail until this point; after the store, a later MT
    ** activation or root flush observes every object and edge in the run.
    */
    oldhead = lj_tg_gcroot_pending_acq(tg);
    gc_root_set_next_rel(tail, oldhead);
    lj_tg_gcroot_pending_store_transition_rel(tg, oldhead, head);
    return;
  }
  oldhead = lj_tg_gcroot_pending_acq(tg);
  do {
    gc_root_set_next_rel(tail, oldhead);
  } while (!lj_tg_gcroot_pending_cas(tg, &oldhead, head));
}

void lj_gc_linkobj_new_after_main(global_State *g, GCobj *o)
{
  TGState *tg = lj_thr_get_tg();
  GCobj *head;
  if (!tg || tg->gl != g || lj_tg_flags_test_acq(tg, TGF_DEAD)) {
    lj_gc_linkobj_after(g, obj2gco(mainthread_acq(g)), o);
    return;
  }
  if (LJ_LIKELY(tg == g->main_tg && mt_active_acq(g) == 0 &&
		mt_entering_acq(g) == 0 && gc2_n_workers_acq(g) == 0)) {
    head = lj_tg_gcroot_pending_after_main_acq(tg);
    gc_root_set_next_rel(o, head);
    lj_tg_gcroot_pending_after_main_store_transition_rel(tg, head, o);
    return;
  }
  head = lj_tg_gcroot_pending_after_main_acq(tg);
  do {
    gc_root_set_next_rel(o, head);
  } while (!lj_tg_gcroot_pending_after_main_cas(tg, &head, o));
}

void lj_gc_linkobj_after(global_State *g, GCobj *anchor, GCobj *o)
{
  GCRef *p;
  GCobj *head;
  if (!anchor || !o)
    return;
  p = lj_obj_gcwref(anchor);
  do {
    head = gcref_acq(*p);
    gc_root_set_next(o, head);
  } while (!gcref_cas(p, &head, o));
  if (g)
    lj_gcroot_repair_epoch_add(g);
}

/* Allocate new GC object and link it to the root set. */
void * LJ_FASTCALL lj_mem_newgco(lua_State *L, GCSize size)
{
  global_State *g = G(L);
  GCobj *o = (GCobj *)lj_mem_newgco_raw(L, size, LJ_AF_TRAVERSABLE);
  newwhite(g, o);
  lj_gc_linkobj_new(g, o);
  return o;
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
