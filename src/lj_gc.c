/*
** Garbage collector.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Major portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lj_gc_c
#define LUA_CORE

#if LJ_GC2_PARANOIA
#include <stdio.h>
#include <stdlib.h>
#endif

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

#define GCSTEPSIZE	1024u
#define GCSWEEPMAX	40
#define GCSWEEPCOST	10
#define GCFINALIZECOST	100

static GCSize gc_step_debt_quantum(global_State *g)
{
  UNUSED(g);
  return (GCSize)LJ_GC2_HELPER_IDLE_STEP;
}

static int gc_hard_assist_due_interp(global_State *g, TGState *tg)
{
  if (!lj_gc2_hard_limit_reached(g))
    return 0;
  return !tg || lj_gc2_hard_load(g) < LJ_GC2_ACCT_FLUSH ||
	 lj_tg_local_total_acq(tg) >= LJ_GC2_ACCT_FLUSH;
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

/* Macros to set GCobj colors and flags. */
#define white2gray(x)		(lj_obj_cleargcflags((x), LJ_GC_WHITES))
#define gray2black(x)		(lj_obj_addgcflags((x), LJ_GC_BLACK))
#define isfinalized(u)		(lj_obj_gcflags(obj2gco(u)) & LJ_GC_FINALIZED)

static void gc_root_wait_no_l(void)
{
  (void)lj_thr_retry_yield(NULL);
}

/* -- Mark phase ---------------------------------------------------------- */

/*
** Legacy marking can overlap standalone minor-root GC2 cycles. Suppress the
** GC2 bridge only while the legacy collector has not explicitly opened its
** full-GC mark bridge; once legacy is blackening objects for a real mark pass,
** the exact same objects and raw allocations must be mirrored into GC2.
*/
void lj_gc_arena_markobj(global_State *g, GCobj *o)
{
  if (!lj_gc2_minor_roots_skip_bridge_mark(g) ||
      gc2_legacy_mark_bridge_acq(g))
    lj_gc2_markobj(g, o);
}

void lj_gc_arena_markmem(global_State *g, void *p)
{
  if (!lj_gc2_minor_roots_skip_bridge_mark(g) ||
      gc2_legacy_mark_bridge_acq(g))
    (void)lj_gc2_markmem(g, p);
}

static void gc_mark_strtab_mem(global_State *g)
{
  StrTabHdr *hdr;
  hdr = lj_str_tabh_acq(g);
  if (hdr)
    lj_gc_arena_markmem(g, hdr);
  for (hdr = lj_str_retired_head_acq(g);
       hdr != NULL;
       hdr = lj_str_retired_next_acq(hdr))
    lj_gc_arena_markmem(g, hdr);
}

static void gc_mark_tab_retired_mem(global_State *g)
{
  TabNodeRetire *ret;
  TabArrayRetire *aret;
  for (ret = lj_tab_node_retired_head_acq(g);
       ret != NULL;
       ret = lj_tab_node_retired_next_acq(ret)) {
    lj_gc_arena_markmem(g, ret);
    if (lj_tab_node_retired_armed_acq(ret))
      lj_gc_arena_markmem(g,
			  lj_tab_node_hdrw(lj_tab_node_retired_node_acq(ret)));
  }
  for (aret = lj_tab_array_retired_head_acq(g);
       aret != NULL;
       aret = lj_tab_array_retired_next_acq(aret)) {
    lj_gc_arena_markmem(g, aret);
    if (lj_tab_array_retired_armed_acq(aret))
      lj_gc_arena_markmem(g,
			  lj_tab_array_hdrw(lj_tab_array_retired_array_acq(aret)));
  }
}

static void gc_arena_rebuild_free(global_State *g)
{
  TGState *tg = G2TG(g);
  if (tg && lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL))
    lj_arena_alloc_rebuild_free(&tg->alloc);
}

static int gc_arena_sweep_ready(global_State *g)
{
  return lj_gc2_sweep_bridge_can_progress(g);
}

static int gc_arena_sweep_needs_prepare(global_State *g)
{
  if (!gc_arena_sweep_ready(g))
    return 0;
  return lj_gc2_sweep_needs_prepare(g);
}

static int gc_arena_sweep_pending(global_State *g)
{
  if (!gc_arena_sweep_ready(g))
    return 0;
  return lj_gc2_sweep_pending(g);
}

static void gc_arena_preserve_root_chain(global_State *g)
{
  GCobj *o;
  (void)lj_gc_flush_root_pending(g);
  for (o = lj_gc_root_acq(g); o != NULL; o = lj_obj_gcw_acq(o)) {
    /*
    ** The root list is still the legacy ownership spine while GC2 owns arena
    ** liveness. At the sweep boundary a root object can be discovered after
    ** MARK/WEAK have closed; preserving only the root cell would leave its
    ** table/proto/closure children reclaimable. Newly preserved traversable
    ** roots are therefore traced immediately before owner sweep reuses cells.
    */
    (void)lj_gc2_preserve_sweep_root(g, o);
  }
}

static uint32_t gc_arena_finish_sweep_boundary(global_State *g, int drain)
{
  uint32_t total = 0;
  if (!gc_arena_sweep_ready(g)) {
    gc_arena_rebuild_free(g);
    return 0;
  }
  lj_gc2_sweep_prepare_bridge_boundary(g, gc_arena_preserve_root_chain);
  do {  /* 05 section 5.6.3 worker-owned sweep bridge. */
    uint32_t swept = lj_gc2_worker_drain(g, LJ_GC2_SWEEP_BATCH);
    if (swept > ~(uint32_t)0 - total)
      total = ~(uint32_t)0;
    else
      total += swept;
    if (!drain || swept == 0)
      break;
  } while (1);
  return total;
}

static void gc_arena_fullgc_drain_sweep(global_State *g)
{
  if (g->gc.state == GCSsweep && gc_arena_sweep_pending(g))
    (void)gc_arena_finish_sweep_boundary(g, 1);
}

#ifdef LUA_USE_ASSERT
static void gc_arena_verify_marked(global_State *g, GCobj *o)
{
  TGState *tg = G2TG(g);
  int marked;
  if (!tg || !lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL))
    return;
  if (o->gch.gct == 0 || o->gch.gct < ~LJ_TSTR ||
      o->gch.gct > ~LJ_TUDATA || o->gch.gct == ~LJ_TSTR)
    return;  /* Deferred/stale bodies and string-table-owned bodies. */
  marked = lj_gc2_ismarked(g, o);
  if (marked < 0)
    return;  /* Custom aligned objects need allocation-base marking first. */
  lj_assertG(marked != 0,
	     "unmarked arena object at verify boundary o=%p gct=%d marked=%02x",
	     (void *)o, o->gch.gct, lj_obj_gcflags(o));
}

static void gc_arena_verify_sweep_boundary(global_State *g)
{
  TGState *tg = G2TG(g);
  GCobj *o;
  if (!tg || !lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL) ||
      !gc_arena_sweep_ready(g))
    return;
  (void)lj_gc_flush_root_pending(g);
  for (o = lj_gc_root_acq(g); o != NULL; o = lj_obj_gcw_acq(o)) {
    gc_arena_verify_marked(g, o);
    if (o->gch.gct == ~LJ_TTHREAD) {
      GCobj *uv;
      for (uv = lj_state_openupval_acq(gco2th(o)); uv != NULL;
	   uv = lj_obj_gcw_acq(uv))
	gc_arena_verify_marked(g, uv);
    }
  }
}
#else
#define gc_arena_verify_sweep_boundary(g)	((void)0)
#endif

#if LJ_GC2_PARANOIA
static void gc2_paranoia_fail(const char *what, const void *p)
{
  fprintf(stderr, "LuaJIT GC2 PARANOIA: missing mark for %s at %p\n",
	  what, p);
  abort();
}

static int gc2_paranoia_liveobj(GCobj *o)
{
  uint8_t flags = lj_obj_gcflags(o);
  return !iswhite(o) || (flags & (LJ_GC_FIXED|LJ_GC_SFIXED));
}

static void gc2_paranoia_checkmem(global_State *g, void *p, const char *what)
{
  int marked = lj_gc2_ismarkedmem(g, p);
  if (marked == 0)
    gc2_paranoia_fail(what, p);
}

static void gc2_paranoia_checkobj(global_State *g, GCobj *o, const char *what)
{
  int marked;
  if (!o || !gc2_paranoia_liveobj(o))
    return;
  marked = lj_gc2_ismarked(g, o);
  if (marked == 0)
    gc2_paranoia_fail(what, o);
}

static void gc2_paranoia_checktab(global_State *g, GCtab *t)
{
  void *arraymem;
  if (!gc2_paranoia_liveobj(obj2gco(t)))
    return;
  gc2_paranoia_checkobj(g, obj2gco(t), "table");
  arraymem = lj_tab_array_mem_acq(t);
  if (arraymem)
    gc2_paranoia_checkmem(g, arraymem, "table array");
  {
    MSize hmask;
    Node *node = lj_tab_node_snapshot_acq(t, &hmask);
    if (hmask > 0)
      gc2_paranoia_checkmem(g, lj_tab_node_hdrw(node), "table node");
  }
}

static void gc2_paranoia_checkthread(global_State *g, lua_State *th)
{
  GCobj *uv;
  if (!gc2_paranoia_liveobj(obj2gco(th)))
    return;
  gc2_paranoia_checkobj(g, obj2gco(th), "thread");
  gc2_paranoia_checkmem(g, tvref(th->stack), "thread stack");
  for (uv = lj_state_openupval_acq(th); uv != NULL; uv = lj_obj_gcw_acq(uv))
    gc2_paranoia_checkobj(g, uv, "open upvalue");
}

static void gc2_paranoia_check_udata(global_State *g, GCudata *ud)
{
  uint8_t udtype;
  if (!gc2_paranoia_liveobj(obj2gco(ud)))
    return;
  gc2_paranoia_checkobj(g, obj2gco(ud), "userdata");
  udtype = lj_udata_udtype_acq(ud);
  if (LJ_HASBUFFER && udtype == UDTYPE_BUFFER) {
    SBufExt *sbx = (SBufExt *)uddata(ud);
    if (!sbufiscoworborrow(sbx))
      gc2_paranoia_checkmem(g, lj_buf_bptr_acq((SBuf *)sbx),
			     "buffer userdata data");
  }
#if LJ_HASFFI
  if (udtype == UDTYPE_FFI_CLIB) {
    CLibrary *cl = (CLibrary *)uddata(ud);
    CLibCacheEntry *e;
    GCtab *cache_env = lj_clib_cache_env_acq(cl);
    if (cache_env)
      gc2_paranoia_checkobj(g, obj2gco(cache_env),
			    "FFI CLibrary cache env");
    for (e = lj_clib_cache_head_acq(cl);
	 e != NULL;
	 e = lj_clib_cache_next_acq(e)) {
      GCstr *name = lj_clib_cache_name_acq(e);
      TValue tv;
      gc2_paranoia_checkmem(g, e, "FFI CLibrary cache entry");
      if (name)
	gc2_paranoia_checkobj(g, obj2gco(name), "FFI CLibrary cache key");
      lj_clib_cache_val_acq(&tv, e);
      if (tvisgcv(&tv))
	gc2_paranoia_checkobj(g, gcV(&tv), "FFI CLibrary cache value");
    }
  }
#endif
}

static void gc2_paranoia_checkone(global_State *g, GCobj *o)
{
  if (!gc2_paranoia_liveobj(o))
    return;
  switch (~o->gch.gct) {
  case LJ_TTAB:
    gc2_paranoia_checktab(g, gco2tab(o));
    break;
  case LJ_TTHREAD:
    gc2_paranoia_checkthread(g, gco2th(o));
    break;
  case LJ_TUDATA:
    gc2_paranoia_check_udata(g, gco2ud(o));
    break;
  default:
    gc2_paranoia_checkobj(g, o, "object");
    break;
  }
}

static void gc2_paranoia_check_strtab(global_State *g)
{
  MSize i;
  StrTabHdr *hdr;
  GCRef *strtab;
  hdr = lj_str_tabh_acq(g);
  if (!hdr)
    return;
  strtab = hdr->bucket;
  for (i = 0; i <= hdr->mask; i++) {
    GCobj *o;
    for (o = lj_str_hashhead_acq(&strtab[i]); o != NULL;
	 o = lj_str_next_acq(o))
      gc2_paranoia_checkobj(g, o, "string");
  }
}

static void gc2_paranoia_check_finalizer_obj(global_State *g, GCobj *o)
{
  gc2_paranoia_checkone(g, o);
}

static void gc2_paranoia_check_roots(global_State *g)
{
  GCobj *o;
  (void)lj_gc_flush_root_pending(g);
  for (o = lj_gc_root_acq(g); o != NULL; o = lj_obj_gcw_acq(o))
    gc2_paranoia_checkone(g, o);
  lj_gc2_finalizer_mark_all(g, gc2_paranoia_check_finalizer_obj);
}

static void gc2_paranoia_check_rawroots(global_State *g)
{
  StrTabHdr *hdr;
  TabNodeRetire *ret;
  TabArrayRetire *aret;
  lj_gc2_smr_read_enter(g);
  hdr = lj_str_tabh_acq(g);
  if (hdr)
    gc2_paranoia_checkmem(g, hdr, "string table");
  for (hdr = lj_str_retired_head_acq(g);
       hdr != NULL;
       hdr = lj_str_retired_next_acq(hdr))
    gc2_paranoia_checkmem(g, hdr, "retired string table");
  for (ret = lj_tab_node_retired_head_acq(g);
       ret != NULL;
       ret = lj_tab_node_retired_next_acq(ret)) {
    gc2_paranoia_checkmem(g, ret, "retired table node record");
    if (lj_tab_node_retired_armed_acq(ret))
      gc2_paranoia_checkmem(g,
			    lj_tab_node_hdrw(lj_tab_node_retired_node_acq(ret)),
			    "retired table nodes");
  }
  for (aret = lj_tab_array_retired_head_acq(g);
       aret != NULL;
       aret = lj_tab_array_retired_next_acq(aret)) {
    gc2_paranoia_checkmem(g, aret, "retired table array record");
    if (lj_tab_array_retired_armed_acq(aret))
      gc2_paranoia_checkmem(g,
			    lj_tab_array_hdrw(lj_tab_array_retired_array_acq(aret)),
			    "retired table array");
  }
#if LJ_64
  gc2_paranoia_checkmem(g, mref(g->gc.lightudseg, uint32_t),
			"lightuserdata segments");
#endif
  gc2_paranoia_checkmem(g, g->tmpbuf.b, "global tmpbuf");
  {
    TGState *tg = gc2_tg_list_acq(g);
    if (!tg)
      tg = G2TG(g);
    for (; tg != NULL; tg = lj_tg_next_acq(tg))
      gc2_paranoia_checkmem(g, tg->tmpbuf.b, "thread tmpbuf");
  }
#if LJ_HASFFI
  {
    CTState *cts = ctype_ctsG(g);
    if (cts) {
      CTypeTab *ctret;
      GCRef *meta = ctype_metamap_acq(cts);
      uint64_t *cbblack = ctype_cbblack_acq(cts);
      gc2_paranoia_checkmem(g, cts, "ctype state");
      gc2_paranoia_checkmem(g, ctype_tabh_acq(cts), "ctype table");
      gc2_paranoia_checkmem(g, meta, "ctype metatype side map");
      gc2_paranoia_checkmem(g, cbblack, "ctype callback blacklist");
      for (ctret = ctype_retiredtab_acq(cts);
	   ctret != NULL;
	   ctret = ctype_tab_retired_next_acq(ctret)) {
	gc2_paranoia_checkmem(g, ctret, "retired ctype table");
      }
      {
	CLibCacheEntry *ce;
	for (ce = lj_clib_cache_retired_head_acq(g);
	     ce != NULL;
	     ce = lj_clib_cache_retired_next_acq(ce)) {
	  GCstr *name = lj_clib_cache_name_acq(ce);
	  TValue tv;
	  gc2_paranoia_checkmem(g, ce, "retired FFI CLibrary cache entry");
	  if (name)
	    gc2_paranoia_checkobj(g, obj2gco(name),
				  "retired FFI CLibrary cache key");
	  lj_clib_cache_val_acq(&tv, ce);
	  if (tvisgcv(&tv))
	    gc2_paranoia_checkobj(g, gcV(&tv),
				  "retired FFI CLibrary cache value");
	}
      }
      gc2_paranoia_checkmem(g, ctype_cb_cbid_acq(cts), "callback ids");
      gc2_paranoia_checkmem(g, ctype_cb_owner_acq(cts), "callback owners");
      gc2_paranoia_checkmem(g, ctype_cb_func_acq(cts), "callback functions");
    }
  }
#endif
#if LJ_HASJIT
  {
    jit_State *J = G2J(g);
    MCodeRetire *mcret;
    TraceVec *tv = tracevec_acq(J);
    if (tv)
      gc2_paranoia_checkmem(g, tv, "trace vector");
    for (tv = tracevec_retired_head_acq(J);
	 tv != NULL;
	 tv = tracevec_retired_next_acq(tv))
      gc2_paranoia_checkmem(g, tv, "retired trace vector");
    for (mcret = mcode_retired_head_acq(J);
	 mcret != NULL;
	 mcret = mcode_retired_next_acq(mcret))
      gc2_paranoia_checkmem(g, mcret, "retired mcode record");
    gc2_paranoia_checkmem(g, J->irbuf ? J->irbuf + J->irbotlim : NULL,
			  "IR buffer");
    gc2_paranoia_checkmem(g, J->snapbuf, "snapshot buffer");
    gc2_paranoia_checkmem(g, J->snapmapbuf, "snapshot map buffer");
  }
#endif
  lj_gc2_smr_read_leave(g);
}

static void gc2_paranoia_check_fixpoint(global_State *g)
{
  if (lj_gc2_minor_roots_active(g))
    return;
  gc2_paranoia_check_roots(g);
  gc2_paranoia_check_strtab(g);
  gc2_paranoia_check_rawroots(g);
}
#else
#define gc2_paranoia_check_fixpoint(g)	((void)0)
#endif

static void gc_mark(global_State *g, GCobj *o);
static void gc_traverse_thread(global_State *g, lua_State *th);

/* Mark a TValue (if needed). */
#define gc_marktv(g, tv) \
  { \
    if (lj_tv_gcref_type_match((tv)) && tvisgcv(tv)) { \
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

static void gc_mark_tg_roots(global_State *g, TGState *tg)
{
  TValue tv;
  if (!tg || lj_tg_flags_test_acq(tg, TGF_DEAD))
    return;
  /*
  ** x64 JIT helpers materialize TValue arguments in these per-TG slots before
  ** entering C. They are part of the trace/native root set: a concurrent
  ** collector can otherwise miss freshly allocated keys or values while they are
  ** between machine registers and helper publication.
  */
  if (lj_tg_load_jit_base(tg) == NULL && lj_tg_vmstate_load_acq(tg) <= 0)
    return;
  lj_tv_load_acq(&tv, &tg->tmptv);
  gc_marktv(g, &tv);
  lj_tv_load_acq(&tv, &tg->tmptv2);
  gc_marktv(g, &tv);
}

static void gc_mark_primary_root(global_State *g, GCobj *o)
{
  if (!o || LJ_UNLIKELY(o->gch.gct == 0))
    return;
  /* Lock-free SMR preservation can leave a primary root non-white between
  ** cycles. At mark-cycle start the gray lists have just been reset, so force
  ** primary roots into the new frontier even if their legacy color is stale.
  */
  lj_gc_arena_markobj(g, o);
  if (!iswhite(o))
    makewhite(g, o);
  gc_mark(g, o);
}

static void gc_mark_primary_root_unique(global_State *g, GCobj *o,
					GCobj **seen, MSize *nseen)
{
  MSize i;
  if (!o)
    return;
  for (i = 0; i < *nseen; i++)
    if (seen[i] == o)
      return;
  seen[(*nseen)++] = o;
  gc_mark_primary_root(g, o);
}

static void gc_normalize_legacy_colors(global_State *g)
{
  GCobj *o;
  (void)lj_gc_flush_root_pending(g);
  for (o = lj_gc_root_acq(g); o != NULL; o = lj_obj_gcw_acq(o)) {
    if (LJ_LIKELY(o->gch.gct != 0) && !iswhite(o)) {
      /* Legacy marking assumes sweep returned every collectable object to
      ** white. SMR-preserved bodies can intentionally survive outside that
      ** path, so normalize colors before rebuilding the mark frontier from
      ** roots. This is a mark-cycle start pass, not a mutator fast path.
      */
      makewhite(g, o);
    }
  }
}

static int gc_mark_rescan_pending_set(GCobj *o)
{
  uint8_t old = la_or8_rlx(&o->gch.marked, LJ_GC_NEEDSCAN);
  return (old & LJ_GC_NEEDSCAN) == 0;
}

void lj_gc_preserveobj_legacy(global_State *g, GCobj *o)
{
  /* SMR-retired GC bodies can outlive their semantic reachability. Preserve
  ** the body itself from legacy list sweep without recursively marking the
  ** object's references; stale lock-free readers may hold this exact body, but
  ** the body is not a root for the Lua object graph.
  */
  lj_gc_arena_markobj(g, o);
  lj_obj_cleargcflags(o, LJ_GC_WHITES);
}

void lj_gc_markobj_legacy(global_State *g, GCobj *o)
{
  if (!g || !o || LJ_UNLIKELY(o->gch.gct == 0))
    return;
  /*
  ** Active GC2 birth marking can make a proto non-white before constructor
  ** edges queue its traversal. When the legacy mark bridge is also active,
  ** feed that proto into the legacy frontier instead of only preserving its
  ** body from sweep.
  */
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
    lj_gc_arena_markobj(g, o);
    if (iswhite(o)) {
      gc_mark(g, o);
    } else if (isgray(o)) {
      return;  /* Already queued on the legacy frontier. */
    } else if (gc_mark_rescan_pending_set(o)) {
      lj_gc_list_push_rel(&g->gc.gray, o);
    }
  } else {
    lj_gc_preserveobj_legacy(g, o);
  }
}

#if LJ_HASFFI
static void gc_mark_clib_retired_cache(global_State *g)
{
  CLibCacheEntry *e;
  for (e = lj_clib_cache_retired_head_acq(g);
       e != NULL;
       e = lj_clib_cache_retired_next_acq(e)) {
    GCstr *name = lj_clib_cache_name_acq(e);
    TValue tv;
    lj_gc_arena_markmem(g, e);
    if (name)
      gc_mark_str(g, name);
    lj_clib_cache_val_acq(&tv, e);
    gc_marktv(g, &tv);
  }
}

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
  for (;;) {
    uint8_t next;
    if (!(old & LJ_GC_WHITES))
      return 0;
    /*
    ** Stock LuaJIT has one legacy marker, so callers assert that an object is
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

/* Mark a white GCobj. */
static void gc_mark(global_State *g, GCobj *o)
{
  int gct = o->gch.gct;
  if (LJ_UNLIKELY(gct == 0))
    return;  /* Body destructor already ran via GC2 arena sweep. */
  if (LJ_UNLIKELY(!gc_mark_claim_white(g, o)))
    return;
  lj_gc_arena_markobj(g, o);
  if (LJ_UNLIKELY(gct == ~LJ_TUDATA)) {
    GCudata *ud = gco2ud(o);
    uint8_t udtype = lj_udata_udtype_acq(ud);
    GCtab *mt = lj_udata_metatable_acq(ud);
    GCtab *env = lj_udata_env_acq(ud);
    gray2black(o);  /* Userdata are never gray. */
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
      ref = gcref_acq(sbx->cowref);
      if (sbufiscow(sbx) && ref)
	gc_markobj(g, ref);
      ref = gcref_acq(sbx->dict_str);
      if (ref)
	gc_markobj(g, ref);
      ref = gcref_acq(sbx->dict_mt);
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
	  gc_marktv(g, &tv);
	}
      }
      if (child)
	gc_markobj(g, obj2gco(child));  /* 09 section 9.2 child stack. */
    }
  } else if (LJ_UNLIKELY(gct == ~LJ_TUPVAL)) {
    GCupval *uv = gco2uv(o);
    TValue tv;
    lj_tv_load_acq(&tv, uvval(uv));
    gc_marktv(g, &tv);
    if (uv->closed)
      gray2black(o);  /* Closed upvalues are never gray. */
  } else if (gct != ~LJ_TSTR && gct != ~LJ_TCDATA) {
    lj_assertG(gct == ~LJ_TFUNC || gct == ~LJ_TTAB ||
	       gct == ~LJ_TTHREAD || gct == ~LJ_TPROTO || gct == ~LJ_TTRACE,
	       "bad GC type %d", gct);
    lj_gc_list_push_rel(&g->gc.gray, o);
  }
}

#if LJ_HASFFI
static void gc_finreg_markobj(global_State *g, GCobj *o)
{
  lj_gc_arena_markobj(g, o);
  if (iswhite(o))
    gc_mark(g, o);
}

static void gc_finreg_marktv(global_State *g, cTValue *tv)
{
  gc_marktv(g, tv);
}
#endif

/* Mark GC roots. */
static void gc_mark_fixedstr(global_State *g)
{
  MSize i;
  StrTabHdr *hdr;
  GCRef *strtab;
  hdr = lj_str_tabh_acq(g);
  if (!hdr)
    return;
  strtab = hdr->bucket;
  for (i = 0; i <= hdr->mask; i++) {
    GCobj *o;
    for (o = lj_str_hashhead_acq(&strtab[i]); o != NULL;
	 o = lj_str_next_acq(o))
      if (lj_obj_gcflags(o) & (LJ_GC_FIXED|LJ_GC_SFIXED))
	lj_gc_arena_markobj(g, o);
  }
}

static void gc_mark_threading_live(global_State *g)
{
  LJThreadLive *node;
  for (node = lj_thread_live_head_acq(g);
       node != NULL;
       node = lj_thread_live_next_acq(node)) {
    GCobj *o = gcref_acq(node->ud);
    if (o && o->gch.gct == ~LJ_TUDATA &&
	lj_udata_udtype_acq(gco2ud(o)) == UDTYPE_THREAD) {
      LJThread *th = (LJThread *)uddata(gco2ud(o));
      TValue *roots = lj_thread_start_roots_acq(th);
      uint32_t i, n = lj_thread_start_root_count_acq(th);
      gc_markobj(g, o);
      lj_gc_arena_markmem(g, roots);
      if (roots) {
	for (i = 0; i < n; i++) {
	  TValue tv;
	  lj_tv_load_acq(&tv, &roots[i]);
	  gc_marktv(g, &tv);
	}
      }
    }
  }
}

static void gc_mark_threading_states(global_State *g)
{
  lua_State *th;
  uint32_t n = 0;
  for (th = (lua_State *)la_loadptr_acq((void *const *)&g->threading_states);
       th != NULL;
       th = (lua_State *)la_loadptr_acq((void *const *)&th->thread_next)) {
    gc_markobj(g, th);
    /*
    ** Joined/suspended registry states can already be gray from an earlier
    ** root path. Classic propagation then may not be the point that mirrors
    ** their raw stack allocation into GC2's arena bitmap. Ownerless stacks are
    ** stable here, so scan them at the registry root edge as well.
    */
    if (lj_state_owner_acq(th) == 0)
      gc_traverse_thread(g, th);
    if (++n >= LJ_GC2_ROOT_SCAN_LIMIT)
      break;
  }
}

static void gc_mark_gcroot(global_State *g)
{
  ptrdiff_t i;
  lj_gc2_smr_read_enter(g);
  for (i = 0; i < GCROOT_MAX; i++) {
    GCobj *o = lj_gcroot_acq(g, (GCRootID)i);
    if (o != NULL)
      gc_markobj(g, o);
  }
  gc_mark_threading_live(g);
  gc_mark_threading_states(g);
  gc_mark_fixedstr(g);
  gc_mark_strtab_mem(g);
  gc_mark_tab_retired_mem(g);
#if LJ_64
  lj_gc_arena_markmem(g, mref(g->gc.lightudseg, uint32_t));
#endif
  lj_gc_arena_markmem(g, g->tmpbuf.b);
  {
    TGState *tg = gc2_tg_list_acq(g);
    int listed = tg != NULL;
    if (!tg)
      tg = G2TG(g);
    for (; tg != NULL; tg = lj_tg_next_acq(tg)) {
      lj_gc_arena_markmem(g, tg->tmpbuf.b);
      gc_mark_tg_roots(g, tg);
    }
    if (listed && g->main_tg)
      gc_mark_tg_roots(g, g->main_tg);
  }
#if LJ_HASFFI
  {
    CTState *cts = ctype_ctsG(g);
    if (cts) {
      CTypeTab *ctret;
      GCRef *meta = ctype_metamap_acq(cts);
      uint64_t *cbblack = ctype_cbblack_acq(cts);
      TValue *func;
      lua_State **owner;
      lj_gc_arena_markmem(g, cts);
      lj_gc_arena_markmem(g, ctype_tabh_acq(cts));
      for (ctret = ctype_retiredtab_acq(cts);
	   ctret != NULL;
	   ctret = ctype_tab_retired_next_acq(ctret)) {
	lj_gc_arena_markmem(g, ctret);
      }
      lj_gc_arena_markmem(g, meta);
      lj_gc_arena_markmem(g, cbblack);
      if (meta) {
	MSize i, n = ctype_metamap_size_acq(cts);
	for (i = 0; i < n; i++) {
	  GCobj *o = ctype_metamap_obj_acq(meta, i);
	  if (o)
	    gc_markobj(g, o);
	}
      }
      gc_mark_clib_retired_cache(g);
      lj_gc2_finreg_cdata_mark_roots(g, gc_finreg_markobj,
				      lj_gc_arena_markmem, gc_finreg_marktv);
      lj_gc_arena_markmem(g, ctype_cb_cbid_acq(cts));
      owner = ctype_cb_owner_acq(cts);
      lj_gc_arena_markmem(g, owner);
      if (owner) {
	MSize i, n = ctype_cb_sizeid_acq(cts);
	for (i = 0; i < n; i++) {
	  lua_State *th = ctype_cb_owner_slot_acq(owner, i);
	  if (th)
	    gc_markobj(g, obj2gco(th));
	}
      }
      func = ctype_cb_func_acq(cts);
      lj_gc_arena_markmem(g, func);
      if (func) {
	MSize i, n = ctype_cb_sizeid_acq(cts);
	for (i = 0; i < n; i++) {
	  TValue tv;
	  lj_tv_load_acq(&tv, &func[i]);
	  gc_marktv(g, &tv);
	}
      }
    }
  }
#endif
#if LJ_HASJIT
  {
    jit_State *J = G2J(g);
    lj_trace_markvecs(g, 0);
    lj_mcode_markretired(g, 0);
    lj_gc_arena_markmem(g, J->irbuf ? J->irbuf + J->irbotlim : NULL);
    lj_gc_arena_markmem(g, J->snapbuf);
    lj_gc_arena_markmem(g, J->snapmapbuf);
  }
#endif
  lj_gc2_smr_read_leave(g);
}

static void gc2_mark_legacy_live_root_spine(global_State *g)
{
  GCobj *o;
  StrTabHdr *hdr;
  (void)lj_gc_flush_root_pending(g);
  /*
  ** This is a semantic mark bridge, not a diagnostic count. Walk the full
  ** legacy ownership spine so large heaps do not lose live objects merely
  ** because the stats path caps its root-spine count.
  */
  for (o = lj_gc_root_acq(g); o != NULL; o = lj_obj_gcw_acq(o)) {
    uint8_t flags;
    if (LJ_UNLIKELY(o->gch.gct == 0))
      continue;
    flags = lj_obj_gcflags(o);
    if (iswhite(o) && !(flags & (LJ_GC_FIXED|LJ_GC_SFIXED)))
      continue;
    (void)lj_gc2_markobj_nolegacy(g, o);
    if (o->gch.gct == ~LJ_TTAB) {
      GCtab *t = gco2tab(o);
      void *arraymem = lj_tab_array_mem_acq(t);
      MSize hmask;
      Node *node;
      if (arraymem)
	(void)lj_gc2_markmem(g, arraymem);
      node = lj_tab_node_snapshot_acq(t, &hmask);
      if (hmask > 0)
	(void)lj_gc2_markmem(g, lj_tab_node_hdrw(node));
    }
  }
  hdr = lj_str_tabh_acq(g);
  if (hdr) {
    MSize i;
    GCRef *strtab = hdr->bucket;
    (void)lj_gc2_markmem(g, hdr);
    for (i = 0; i <= hdr->mask; i++) {
      for (o = lj_str_hashhead_acq(&strtab[i]); o != NULL;
	   o = lj_str_next_acq(o)) {
	uint8_t flags = lj_obj_gcflags(o);
	if (!iswhite(o) || (flags & (LJ_GC_FIXED|LJ_GC_SFIXED)))
	  (void)lj_gc2_markobj_nolegacy(g, o);
      }
    }
  }
}

/* Start a GC cycle and mark the root set. */
static void gc_mark_start(global_State *g)
{
  lua_State *mainL = mainthread_acq(g);
  lua_State *vmL = vmthread_acq(g);
  GCobj *seen[GCROOT_MAX + 4];
  MSize nseen = 0;
  ptrdiff_t i;
  /* The root scan cannot make progress without the required main thread. */
  lj_assertG(mainL != NULL, "missing main thread root");
  if (LJ_UNLIKELY(mainL == NULL))
    return;
  (void)lj_gc_flush_root_pending(g);
  lj_gc2_mark_begin(g);
  /*
  ** This is the only path where GC2 marks intentionally feed legacy color
  ** state. Standalone GC2 cycles leave the latch clear, because their arena
  ** marks are not a legacy sweep frontier and must not make conservative
  ** temporary roots look legacy-live.
  */
  lj_gc2_legacy_mark_bridge_enable(g);
  gc_normalize_legacy_colors(g);
  lj_gc_list_clear_rel(&g->gc.gray);
  lj_gc_list_clear_rel(&g->gc.grayagain);
  lj_gc_list_clear_rel(&g->gc.weak);
  gc_mark_primary_root_unique(g, obj2gco(mainL), seen, &nseen);
  {
    GCtab *env = lj_state_env_acq(mainL);
    if (env)
      gc_mark_primary_root_unique(g, obj2gco(env), seen, &nseen);
  }
  if (vmL != mainL)
    gc_mark_primary_root_unique(g, obj2gco(vmL), seen, &nseen);
  {
    TValue tv;
    lj_tv_load_acq(&tv, lj_registry_ref(g));
    if (tvisgcv(&tv))
      gc_mark_primary_root_unique(g, gcV(&tv), seen, &nseen);
  }
  for (i = 0; i < GCROOT_MAX; i++)
    gc_mark_primary_root_unique(g, lj_gcroot_acq(g, (GCRootID)i),
				seen, &nseen);
  gc_mark_gcroot(g);
  g->gc.state = GCSpropagate;
}

/* Mark open upvalues. */
static void gc_mark_uv(global_State *g)
{
  GCupval *uv;
  for (uv = lj_uv_next_acq(&g->uvhead); uv != &g->uvhead;
       uv = lj_uv_next_acq(uv)) {
    lj_assertG(lj_uv_prev_acq(lj_uv_next_acq(uv)) == uv &&
	       lj_uv_next_acq(lj_uv_prev_acq(uv)) == uv,
	       "broken upvalue chain");
    if (isgray(obj2gco(uv))) {
      TValue tv;
      lj_tv_load_acq(&tv, uvval(uv));
      gc_marktv(g, &tv);
    }
  }
}

static void gc_mark_finalizer_obj(global_State *g, GCobj *o)
{
  makewhite(g, o);  /* Could be from previous GC. */
  (void)lj_gc2_markobj_nolegacy(g, o);
  gc_mark(g, o);
}

static int gc_chain_splice(GCRef *p, GCobj *o)
{
  GCobj *next = lj_obj_gcw_acq(o);
  GCRef oldref, nextref;
  setgcref(oldref, o);
  if (next)
    setgcref(nextref, next);
  else
    setgcrefnull(nextref);
#if LJ_GC64
  return la_cas64(&p->gcptr64, &oldref.gcptr64, nextref.gcptr64,
		  LA_ACQ_REL, LA_ACQ);
#else
  return la_cas32(&p->gcptr32, &oldref.gcptr32, nextref.gcptr32,
		  LA_ACQ_REL, LA_ACQ);
#endif
}

/* Mark userdata/cdata in finalizer queues. */
static void gc_mark_finalizers(global_State *g)
{
  lj_gc2_finalizer_mark_all(g, gc_mark_finalizer_obj);
}

/* -- Propagation phase --------------------------------------------------- */

static int gc_weak_list_has(global_State *g, GCtab *t)
{
  GCobj *want = obj2gco(t);
  GCobj *o;
  for (o = lj_gc_list_head_acq(&g->gc.weak); o != NULL;
       o = lj_tab_gclist_acq(gco2tab(o)))
    if (o == want)
      return 1;
  return 0;
}

/* Traverse a table. */
static int gc_traverse_tab(global_State *g, GCtab *t)
{
  int weak = 0;
  int ffi_fin = 0;
  void *arraymem;
  TValue modev;
  cTValue *mode;
  GCtab *mt = lj_tab_metatable_acq(t);
  arraymem = lj_tab_array_mem_acq(t);
  if (arraymem)
    (void)lj_gc2_markmem(g, arraymem);
  {
    MSize hmask;
    Node *node = lj_tab_node_snapshot_acq(t, &hmask);
    if (hmask > 0)
      (void)lj_gc2_markmem(g, lj_tab_node_hdrw(node));
  }
  if (mt)
    gc_markobj(g, mt);
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
	if (!gc_weak_list_has(g, t))
	  lj_gc_list_push_rel(&g->gc.weak, obj2gco(t));
      }
    }
  }
  if (weak == LJ_GC_WEAK)  /* Nothing to mark if both keys/values are weak. */
    return 1;
  if (!(weak & LJ_GC_WEAKVAL)) {  /* Mark array part. */
    TValue *array;
    MSize i, asize = lj_tab_array_snapshot_acq(t, &array);
    for (i = 0; i < asize; i++) {
      TValue val;
      lj_tv_load_acq(&val, &array[i]);
      if (tvisforward(&val) &&
	  !lj_tab_forwarded_array_slot(t, array, asize, i, &val))
	continue;
      gc_marktv(g, &val);
    }
  }
  {  /* Mark hash part. */
    MSize i, hmask;
    Node *node = lj_tab_node_snapshot_acq(t, &hmask);
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
	  if (!(weak & LJ_GC_WEAKKEY)) gc_marktv(g, &key);
	  if (!(weak & LJ_GC_WEAKVAL)) gc_marktv(g, &val);
	}
      }
    }
  }
  return weak;
}

/* Traverse a function. */
static void gc_traverse_func(global_State *g, GCfunc *fn)
{
  {
    GCtab *env = lj_func_env_acq(fn);
    if (env)
      gc_markobj(g, env);
  }
  if (isluafunc(fn)) {
    uint32_t i, nup = lj_funcL_nupvalues(&fn->l);
    lj_assertG(nup <= funcproto(fn)->sizeuv,
	       "function upvalues out of range");
    gc_markobj(g, funcproto(fn));
    for (i = 0; i < nup; i++)  /* Mark Lua function upvalues. */
      gc_markobj(g, func_uv_acq(&fn->l, i));
  } else {
    uint32_t i, nup = lj_funcC_nupvalues(&fn->c);
    for (i = 0; i < nup; i++) {  /* Mark C function upvalues. */
      TValue tv;
      lj_tv_load_acq(&tv, &fn->c.upvalue[i]);
      gc_marktv(g, &tv);
    }
  }
}

#if LJ_HASJIT
static GCtrace *gc_traceref_safe(global_State *g, TraceNo traceno)
{
  jit_State *J = G2J(g);
  TraceVec *tv = tracevec_acq(J);
  if (traceno == 0 || tv == NULL || (MSize)traceno >= tv->sizetrace)
    return NULL;
  return traceref_fromgco(gcref_acq(tv->slot[traceno]));
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
  if (iswhite(o)) {
    lj_gc_arena_markobj(g, o);
    white2gray(o);
    lj_gc_list_push_rel(&g->gc.gray, o);
  }
}

void lj_gc_mark_trace_slot(global_State *g, uint32_t traceno)
{
  gc_marktrace(g, (TraceNo)traceno);
}

static void gc_mark_proto_for_trace_pc(global_State *g, const BCIns *pc)
{
  GCobj *o;
  if (!pc)
    return;
  (void)lj_gc_flush_root_pending(g);
  for (o = lj_gc_root_acq(g); o != NULL; o = lj_obj_gcw_acq(o)) {
    if (o->gch.gct == ~LJ_TPROTO) {
      GCproto *pt = gco2pt(o);
      const BCIns *bc = proto_bc(pt);
      if (pc >= bc && pc < bc + pt->sizebc) {
	gc_markobj(g, o);
	return;
      }
    }
  }
}

static void gc_mark_trace_snapshot_pcs(global_State *g, GCtrace *T)
{
  SnapShot *snap = trace_snap_acq(T);
  SnapEntry *snapmap = trace_snapmap_acq(T);
  SnapNo i, nsnap = trace_nsnap_acq(T);
  for (i = 0; i < nsnap; i++) {
    SnapShot *s = &snap[i];
    SnapEntry *map = &snapmap[snap_mapofs_acq(s)];
    gc_mark_proto_for_trace_pc(g, snap_pc_acq(&map[snap_nent_acq(s)]));
  }
}

/* Traverse a trace. */
static void gc_traverse_trace(global_State *g, GCtrace *T)
{
  IRIns *irbase;
  IRRef ref;
  if (trace_traceno_acq(T) == 0) return;
  irbase = trace_ir_acq(T);
  for (ref = trace_nk_acq(T); ref < REF_TRUE; ref++) {
    IRIns *ir = &irbase[ref];
    IRIns irs = ir_load_acq(ir);
    if (irs.o == IR_KGC)
      gc_markobj(g, ir_kgc_load_acq(ir));
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
  gc_markobj(g, trace_startptgco_acq(T));
  gc_mark_trace_snapshot_pcs(g, T);
}

/* The current trace is a GC root while not anchored in the prototype (yet). */
#define gc_traverse_curtrace(g)	gc_traverse_trace(g, &G2J(g)->cur)
#else
#define gc_traverse_curtrace(g)	UNUSED(g)
#endif

/* Traverse a prototype. */
static void gc_traverse_proto(global_State *g, GCproto *pt)
{
  ptrdiff_t i;
  gc_mark_str(g, proto_chunkname_acq(pt));
  for (i = -(ptrdiff_t)pt->sizekgc; i < 0; i++)  /* Mark collectable consts. */
    gc_markobj(g, proto_kgc_acq(pt, i));
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

#if LJ_HASJIT
static TGState *gc_thread_active_tg(global_State *g, lua_State *th)
{
  uint32_t owner;
  TGState *tg = NULL;
  if (!g || !th)
    return NULL;
  owner = lj_state_owner_acq(th);
  if (owner != 0 && owner != LJ_THREAD_GCSCAN)
    tg = lj_tg_find_owner(g, owner);
  else if (th == lj_tg_cur_L(g))
    tg = G2TG(g);
  if (!tg || lj_tg_flags_test_acq(tg, TGF_DEAD) ||
      lj_tg_load_cur_L(tg) != th)
    return NULL;
  return tg;
}

static TValue *gc_thread_jit_base(global_State *g, lua_State *th)
{
  TGState *tg = gc_thread_active_tg(g, th);
  return tg ? lj_tg_load_jit_base(tg) : NULL;
}
#endif

static void gc_mark_thread_root_func(global_State *g, GCfunc *fn);

static void gc_mark_jit_frame_funcs(global_State *g, lua_State *th)
{
#if LJ_HASJIT
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
  ** JIT C helpers keep jit_base published while vmstate is C, so no positive
  ** trace-number root exists for gc_mark_gcroot() to follow. The raw stack scan
  ** below keeps value slots live, but frame headers are not normal tagged
  ** values. Mark their function/prototype chain explicitly.
  */
  for (frame = base - 1; frame > bot + LJ_FR2 && frame < max; ) {
    GCobj *fo = frame_gc(frame);
    TValue *prev = frame_prev(frame);
    if (fo && fo->gch.gct == ~LJ_TFUNC)
      gc_mark_thread_root_func(g, &fo->fn);
    if (prev >= frame || prev <= bot + LJ_FR2 || prev >= max)
      break;
    frame = prev;
    if (++n >= LJ_GC2_ROOT_SCAN_LIMIT)
      break;
  }
#else
  UNUSED(g); UNUSED(th);
#endif
}

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

static int gc_thread_is_jit_current(global_State *g, lua_State *th)
{
#if LJ_HASJIT
  /*
  ** Trace/native helpers own the current frame layout until jit_base and the
  ** positive trace vmstate are cleared. Legacy root marking mirrors GC2 here:
  ** preserve raw stack storage and explicitly preserve frame-header function
  ** roots when jit_base is the only published edge.
  */
  {
    TGState *tg = gc_thread_active_tg(g, th);
    return tg &&
      (lj_tg_load_jit_base(tg) != NULL || lj_tg_vmstate_load_acq(tg) > 0);
  }
#else
  UNUSED(g); UNUSED(th);
  return 0;
#endif
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

/* Calculate number of used slots in a live bytecode frame. */
static BCReg gc_cur_topslot(GCproto *pt, const BCIns *pc, uint32_t nres)
{
  BCIns ins = pc[-1];
  if (bc_op(ins) == BC_UCLO)
    ins = pc[bc_j(ins)];
  switch (bc_op(ins)) {
  case BC_CALL: case BC_ITERC:
    return bc_a(ins) + bc_c(ins) + LJ_FR2;
  case BC_CALLM: case BC_CALLMT:
    return bc_a(ins) + bc_c(ins) + nres-1+1+LJ_FR2;
  case BC_RETM:
    return bc_a(ins) + bc_d(ins) + nres-1;
  case BC_TSETM:
    return bc_a(ins) + nres-1;
  default:
    return pt->framesize;
  }
}

static TValue *gc_active_thread_top(lua_State *th, TValue *top)
{
  TValue *bot = tvref(th->stack);
  TValue *max = tvref(th->maxstack);
  TValue *frame;
  if (top > max)
    top = max;
  if (th->base <= bot + 1 + LJ_FR2)
    return top;
  frame = th->base - 1;
  if (frame > bot + LJ_FR2 && frame_islua(frame)) {
    GCproto *pt = gc_func_proto_if_lua(frame_func(frame));
    if (pt) {
      TValue *ltop = th->base + pt->framesize;
      if (ltop > top)
	top = ltop;
    }
  } else if (frame > bot + LJ_FR2 && frame_isc(frame)) {
    TValue *prev = frame_prev(frame);
    if (prev > bot + LJ_FR2 && frame_islua(prev)) {
      GCproto *pt = gc_func_proto_if_lua(frame_func(prev));
      if (pt) {
	void *cf = cframe_raw(th->cframe);
	const BCIns *pc = cf ? cframe_pc(cf) : NULL;
	const BCIns *bc = proto_bc(pt);
	if (pc && pc > bc && pc <= bc + pt->sizebc) {
	  TValue *ctop = prev + 1 + gc_cur_topslot(pt, pc,
						   cframe_multres_n(cf));
	  if (ctop > top)
	    top = ctop;
	}
      }
    }
  }
  return top > max ? max : top;
}

static void gc_mark_thread_root_tv(global_State *g, cTValue *tv);
static void gc_mark_thread_root_tab(global_State *g, GCtab *t);

static void gc_mark_thread_root_proto(global_State *g, GCproto *pt)
{
  GCobj *o;
  if (!pt)
    return;
  o = obj2gco(pt);
  lj_gc_arena_markobj(g, o);
  if (iswhite(o))
    gc_mark(g, o);
  else
    gc_traverse_proto(g, pt);
}

static void gc_mark_thread_root_func(global_State *g, GCfunc *fn)
{
  GCobj *o;
  if (!fn)
    return;
  o = obj2gco(fn);
  lj_gc_arena_markobj(g, o);
  if (iswhite(o)) {
    gc_mark(g, o);
  } else {
    /*
    ** Stack frame functions are primary roots for their prototype graph. SMR
    ** preservation can leave a root non-white without proving its children
    ** were visited in this legacy cycle, so traverse root function edges here
    ** instead of relying on color alone. This keeps live proto string constants
    ** interned and preserves pointer-equality string semantics.
    */
    GCtab *env = lj_func_env_acq(fn);
    if (env)
      gc_mark_thread_root_tab(g, env);
    if (isluafunc(fn)) {
      uint32_t i, nup = lj_funcL_nupvalues(&fn->l);
      lj_assertG(nup <= funcproto(fn)->sizeuv,
		 "function upvalues out of range");
      gc_mark_thread_root_proto(g, funcproto(fn));
      for (i = 0; i < nup; i++)
	gc_markobj(g, func_uv_acq(&fn->l, i));
    } else {
      uint32_t i, nup = lj_funcC_nupvalues(&fn->c);
      for (i = 0; i < nup; i++) {
	TValue tv;
	lj_tv_load_acq(&tv, &fn->c.upvalue[i]);
	gc_marktv(g, &tv);
      }
    }
  }
}

static void gc_mark_thread_root_tv(global_State *g, cTValue *tv)
{
  GCobj *o;
  if (!tvisgcv(tv))
    return;
  o = gcV(tv);
  lj_gc_arena_markobj(g, o);
  if (iswhite(o)) {
    gc_mark(g, o);
  } else if (tvistab(tv)) {
    if (gc_traverse_tab(g, tabV(tv)) > 0)
      black2gray(o);
  } else if (tvisfunc(tv)) {
    gc_mark_thread_root_func(g, funcV(tv));
  } else if (tvisproto(tv)) {
    gc_mark_thread_root_proto(g, protoV(tv));
  }
}

static void gc_mark_thread_root_tab(global_State *g, GCtab *t)
{
  GCobj *o;
  if (!t)
    return;
  o = obj2gco(t);
  lj_gc_arena_markobj(g, o);
  if (iswhite(o))
    gc_mark(g, o);
  else {
    if (gc_traverse_tab(g, t) > 0)
      black2gray(o);
  }
}

/* Traverse a thread object. */
static void gc_traverse_thread(global_State *g, lua_State *th)
{
  GCobj *mt;
  TValue *o, *top = th->top;
  TValue tv;
  MSize used;
  uint32_t owner;
  int owned;
  int remote_current;
  int jit_current;
  lua_State *cur_L;
  lj_gc_arena_markmem(g, tvref(th->stack));
  cur_L = lj_tg_cur_L(g);
  owner = lj_state_owner_acq(th);
  owned = owner != 0 && owner != LJ_THREAD_GCSCAN && th != cur_L;
  remote_current = gc_thread_is_remote_current(g, th);
  jit_current = gc_thread_is_jit_current(g, th);
  if (owned || remote_current || jit_current) {
    if (!owned && !remote_current && jit_current)
      gc_mark_jit_frame_funcs(g, th);
    top = tvref(th->maxstack);
    used = (MSize)(top - tvref(th->stack));
  } else {
    used = gc_traverse_frames(g, th);
  }
  if (!remote_current && !jit_current && th == cur_L &&
      th->base > tvref(th->stack) + 1 + LJ_FR2) {
    top = gc_active_thread_top(th, top);
  } else if (tvref(th->stack) + used > top) {
    top = tvref(th->stack) + used;
  }
  for (o = tvref(th->stack)+1+LJ_FR2; o < top; o++) {
    lj_tv_load_acq(&tv, o);
    gc_mark_thread_root_tv(g, &tv);
  }
  if (!owned && !remote_current && !jit_current && g->gc.state == GCSatomic) {
    top = tvref(th->stack) + th->stacksize;
    for (; o < top; o++)  /* Clear unmarked slots. */
      setnilV(o);
  }
  for (mt = lj_state_openupval_acq(th); mt != NULL; mt = lj_obj_gcw_acq(mt)) {
    lj_gc_arena_markobj(g, mt);
    if (iswhite(mt))
      gc_mark(g, mt);
  }
  {
    GCtab *env = lj_state_env_acq(th);
    gc_mark_thread_root_tab(g, env);
  }
  mt = lj_state_mt_thread_acq(th);
  if (mt != NULL)
    gc_markobj(g, mt);
  if (!owned && th != cur_L)
    lj_state_shrinkstack(th, used);
}

/* Propagate one gray object. Traverse it and turn it black. */
static size_t propagatemark(global_State *g)
{
  GCobj *o = lj_gc_list_head_acq(&g->gc.gray);
  int gct = o->gch.gct;
  int black_rescan = !isgray(o) && isblack(o);
  int rescan = black_rescan && (lj_obj_gcflags(o) & LJ_GC_NEEDSCAN);
  if (LJ_UNLIKELY(!isgray(o) && !black_rescan)) {
    /*
    ** Lock-free publication can leave duplicate/stale nodes on the legacy gray
    ** list after another path has already removed the object from this frontier.
    ** A black duplicate is conservatively re-traversed below; a white/dead entry
    ** is not valid mark work for this cycle and must be unlinked as stale.
    */
    lj_gc_list_pop_head_rel(&g->gc.gray, o);
    return 0;
  }
  lj_assertG(isgray(o) || rescan || black_rescan,
	     "propagation of non-gray object");
  /*
  ** A forced full collection may first finish an already-active legacy mark
  ** cycle before it can start a fresh major GC2 cycle. Objects left gray by a
  ** mutator publication in that older cycle still become legacy-live here, so
  ** mirror the object itself into GC2 at the blackening edge.
  **
  ** GC2 can also hand immutable/birth-marked objects to the legacy gray list as
  ** black+NEEDSCAN rescans. Those are already legacy-live; clear the handoff bit
  ** and traverse the payload without changing their color again.
  **
  ** A stale duplicate gray-list node can also reach the head after another entry
  ** has already blackened the same object. Re-traversing that payload is
  ** conservative and keeps resize-forwarded table edges from depending on which
  ** duplicate list node wins the race to the head.
  */
  (void)lj_gc2_markobj_nolegacy(g, o);
  if (rescan)
    lj_obj_cleargcflags_atomic(o, LJ_GC_NEEDSCAN);
  else if (!black_rescan)
    gray2black(o);
  lj_gc_list_pop_head_rel(&g->gc.gray, o);  /* Remove from gray list. */
  if (LJ_LIKELY(gct == ~LJ_TTAB)) {
    GCtab *t = gco2tab(o);
    int8_t colo = lj_tab_colo_acq(t);
    MSize acap = lj_tab_array_separated_acap_acq(t);
    MSize hmask;
    (void)lj_tab_node_snapshot_acq(t, &hmask);
    if (gc_traverse_tab(g, t) > 0)
      black2gray(o);  /* Keep weak tables gray. */
    return (LJ_MAX_COLOSIZE != 0 && colo ?
	    sizetabcolo((uint32_t)colo & 0x7f) : sizeof(GCtab)) +
	   (acap ? lj_tab_array_bytes(acap) : 0) +
	   (hmask ? lj_tab_node_bytes(hmask) : 0);
  } else if (LJ_LIKELY(gct == ~LJ_TFUNC)) {
    GCfunc *fn = gco2func(o);
    gc_traverse_func(g, fn);
    return isluafunc(fn) ? sizeLfunc((MSize)lj_funcL_nupvalues(&fn->l)) :
			   sizeCfunc((MSize)lj_funcC_nupvalues(&fn->c));
  } else if (LJ_LIKELY(gct == ~LJ_TPROTO)) {
    GCproto *pt = gco2pt(o);
    gc_traverse_proto(g, pt);
    return pt->sizept;
  } else if (LJ_LIKELY(gct == ~LJ_TTHREAD)) {
    lua_State *th = gco2th(o);
    lj_gc_list_push_rel(&g->gc.grayagain, o);
    black2gray(o);  /* Threads are never black. */
    gc_traverse_thread(g, th);
    return sizeof(lua_State) + sizeof(TValue) * th->stacksize;
  } else {
#if LJ_HASJIT
    GCtrace *T = gco2trace(o);
    IRRef nins, nk;
    gc_traverse_trace(g, T);
    nins = trace_nins_acq(T);
    nk = trace_nk_acq(T);
    return ((sizeof(GCtrace)+7)&~7) + (nins-nk)*sizeof(IRIns) +
	   trace_nsnap_acq(T)*sizeof(SnapShot) +
	   trace_nsnapmap_acq(T)*sizeof(SnapEntry);
#else
    lj_assertG(0, "bad GC type %d", gct);
    return 0;
#endif
  }
}

/* Propagate all gray objects. */
static size_t gc_propagate_gray(global_State *g)
{
  size_t m = 0;
  while (lj_gc_list_head_acq(&g->gc.gray) != NULL)
    m += propagatemark(g);
  return m;
}

static int gc2_legacy_mark_complete(global_State *g, lua_State *L)
{
  /*
  ** A late native root can ask GC2 to abort its arena-mark cycle while the
  ** classic collector is already in its mark/atomic phase. Classic colors remain
  ** authoritative for this legacy full collection, so an idle GC2 phase is an
  ** already-closed bridge, not a reason to rerun atomic forever.
  */
  if (!lj_gc2_mark_phase_active(g))
    return 1;
  return lj_gc2_mark_complete(g, L, 64, ~(uint32_t)0);
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
  ** Strings are owned by the intern table and swept through gc_sweepstr().
  ** Traversable arena scans can encounter stale type bytes from reclaimed or
  ** reused bodies; dispatching those through lj_str_free() would double-count
  ** string-table ownership and corrupt g->str.num.
  */
  if (gct == (uint32_t)~LJ_TSTR)
    return 0;
  if (gct >= (uint32_t)~LJ_TSTR && gct <= (uint32_t)~LJ_TUDATA) {
    GCFreeFunc fn = gc_freefunc[gct - (uint32_t)~LJ_TSTR];
    if (fn) {
      fn(g, o);
      o->gch.gct = 0;  /* Body is awaiting bitmap reuse; destructor is done. */
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
  MSize colosz = colo ? ((MSize)(uint8_t)colo & 0x7fu) : 0;
  MSize asize = lj_tab_asize_acq(t);
  MSize acap = lj_tab_acap_acq(t);
  TValue *array = lj_tab_array_acq(t);
  TValue *coloarray = (TValue *)(void *)((char *)(void *)t + sizeof(GCtab));
  Node *node = lj_tab_node_acq(t);
  MSize hmask;
  GCSize bodysize;

  if (LJ_MAX_COLOSIZE != 0 && colosz > LJ_MAX_COLOSIZE)
    return 0;
  if (asize > LJ_MAX_ASIZE || acap > LJ_MAX_ASIZE)
    return 0;
  if (colo > 0) {
    if (array != coloarray || asize > colosz || acap > colosz)
      return 0;
  } else if (array == coloarray) {
    /*
    ** A negative colocated marker means a resize has split the old inline array
    ** from table indexing. A dead table still pointing at the inline storage is
    ** in a transient resize state; ordinary sweep must not free it as separated.
    */
    return 0;
  } else if (array != NULL) {
    MSize hacap;
    if (lj_tab_array_is_retiring(t, array))
      return 0;
    hacap = lj_tab_array_hdr_acap_acq(array);
    if (hacap == 0 || hacap > LJ_MAX_ASIZE)
      return 0;
    if (!gc2_size_fits_mem(g, lj_tab_array_hdrw(array),
			   lj_tab_array_bytes(hacap)))
      return 0;
  }

  if (node == NULL)
    return 0;
  hmask = lj_tab_node_hmask_acq(node);
  if (!gc2_valid_pow2_mask(hmask))
    return 0;
  if (hmask > 0) {
    if (lj_tab_node_is_retiring(node))
      return 0;
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

static void gc2_preserve_pending_finalizer_body(global_State *g, GCobj *o)
{
  /*
  ** FINREG, not ordinary sweep, owns the transition from finalizer-registered
  ** cdata to a freeable body. Preserve the arena cell without traversing the
  ** cdata payload and keep legacy color white so the ordered FINREG P_WEAK scan
  ** can still discover and queue the finalizer.
  */
  (void)lj_gc2_markobj_nolegacy(g, o);
  makewhite(g, o);
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
  if (gct == (uint32_t)~LJ_TTAB && !gc2_valid_tab_obj(g, gco2tab(o)))
    return 0;  /* Stale table header: side-vector sizes are not trustworthy. */
  return gct >= (uint32_t)~LJ_TSTR && gct <= (uint32_t)~LJ_TUDATA &&
	 gc_freefunc[gct - (uint32_t)~LJ_TSTR] != NULL;
}

static int gc2_arena_owned_fnew_body(GCobj *o)
{
  /*
  ** FNEW active-black bump allocation can skip the legacy root spine for fresh
  ** Lua closures and closed local-cell upvalues. Type-local marker bits prove
  ** that narrow body-lifetime ownership state without overloading nextgc, which
  ** remains reserved for root/open-upvalue chains.
  */
  if (o->gch.gct == (uint32_t)~LJ_TFUNC) {
    GCfunc *fn = gco2func(o);
    return isluafunc(fn) && lj_funcL_arenaowned(&fn->l) &&
	   lj_funcL_nupvalues(&fn->l) <= LJ_MAX_UPVAL;
  }
  if (o->gch.gct == (uint32_t)~LJ_TUPVAL) {
    GCupval *uv = gco2uv(o);
    return lj_uv_arenaowned(uv) && uv->closed && uvval(uv) == &uv->tv;
  }
  return 0;
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

uint32_t lj_gc_sweep_gc2_unmarked(global_State *g)
{
  GCRef *p = lj_gc_root_ref(g);
  GCobj *o;
  uint32_t n = 0;
  (void)lj_gc_flush_root_pending(g);
  while ((o = gcref_acq(*p)) != NULL) {
    int marked = lj_gc2_ismarked(g, o);
    if (marked == 0) {
      if (o->gch.gct == 0) {
	if (!gc_chain_splice(p, o)) {
	  gc_root_wait_no_l();
	  continue;
	}
	continue;
      }
      if (LJ_UNLIKELY(gc2_cdata_finalizer_pending(o))) {
	gc2_preserve_pending_finalizer_body(g, o);
	p = lj_obj_gcwref(o);
	continue;
      }
      if (isdead(g, o) && gc2_valid_freeable_obj(g, o)) {
	if (!gc_chain_splice(p, o)) {
	  gc_root_wait_no_l();
	  continue;
	}
	if (!gc2_free_unmarked_obj(g, o))
	  continue;
	n++;
	continue;
      }
    }
    p = lj_obj_gcwref(o);
  }
  return n;
}

static uint32_t gc2_sweep_arena_bodies(global_State *g, GCArena *a,
				       int unmarked_only)
{
  uint32_t i, n = 0;
  if (!g || !a)
    return 0;
  for (i = LJ_AFIRST_CELL; i < LJ_ARENA_CELLS; i++) {
    if (lj_arena_bm_get(a->block, i) &&
	(!unmarked_only || !lj_arena_bm_get(a->mark, i))) {
      GCobj *o = (GCobj *)lj_arena_cellptr(a, i);
      if (gc2_cdata_finalizer_pending(o)) {
	gc2_preserve_pending_finalizer_body(g, o);
	continue;
      }
      if (unmarked_only && gc2_arena_owned_fnew_body(o) &&
	  gc2_valid_freeable_obj(g, o)) {
	if (!gc2_free_unmarked_obj(g, o))
	  continue;
	n++;
	continue;
      }
      if (unmarked_only && gc2_valid_freeable_obj(g, o)) {
	lj_arena_bm_set(a->mark, i);
	continue;
      }
      if ((!unmarked_only || isdead(g, o)) && gc2_valid_freeable_obj(g, o)) {
	lj_gc_unlink_root_obj(g, o);
	if (!gc2_free_unmarked_obj(g, o))
	  continue;
	n++;
      }
    }
  }
  return n;
}

uint32_t lj_gc_sweep_gc2_arena_unmarked(global_State *g, GCArena *a)
{
  return gc2_sweep_arena_bodies(g, a, 1);
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

/* Full sweep of a GC list. */
#define gc_fullsweep(g, p)	gc_sweep(g, (p), ~(uint32_t)0)

/* Partial sweep of a GC list. */
static GCRef *gc_sweep(global_State *g, GCRef *p, uint32_t lim)
{
  /* Mask with other white and LJ_GC_FIXED. Or LJ_GC_SFIXED on shutdown. */
  int ow = otherwhite(g);
  GCobj *o;
  if (p == lj_gc_root_ref(g))
    (void)lj_gc_flush_root_pending(g);
  while ((o = gcref_acq(*p)) != NULL && lim-- > 0) {
    if (LJ_UNLIKELY(o->gch.gct == 0)) {
      if (!gc_chain_splice(p, o)) {
	gc_root_wait_no_l();
	continue;
      }
      continue;  /* Body destructor already ran via GC2 arena sweep. */
    }
    if (o->gch.gct == ~LJ_TTHREAD)  /* Need to sweep open upvalues, too. */
      gc_fullsweep(g, lj_state_openupval_ref(gco2th(o)));
    if (((lj_obj_gcflags(o) ^ LJ_GC_WHITES) & ow)) {  /* Black or current white? */
      lj_assertG(!isdead(g, o) || (lj_obj_gcflags(o) & LJ_GC_FIXED),
		 "sweep of undead object");
      makewhite(g, o);  /* Value is alive, change to the current white. */
      p = lj_obj_gcwref(o);
    } else {  /* Otherwise value is dead, free it. */
      int deferred = gc2_deferred_body_pending(g, o);
      lj_assertG(isdead(g, o) || ow == LJ_GC_SFIXED,
		 "sweep of unlive object");
      if (LJ_UNLIKELY(gc2_cdata_finalizer_pending(o))) {
	gc2_preserve_pending_finalizer_body(g, o);
	p = lj_obj_gcwref(o);
	continue;
      }
      if (LJ_UNLIKELY(!gc2_valid_freeable_obj(g, o))) {
	/*
	** Lock-free root publication can leave an SMR-preserved arena body on the
	** legacy root spine after its destructor has run or after the header has
	** been reused for non-GC state. The legacy sweeper cannot dispatch from
	** that stale gct byte; unlink the ownership-spine entry and leave the body
	** lifetime to arena/SMR reclamation.
	*/
	if (!gc_chain_splice(p, o)) {
	  gc_root_wait_no_l();
	  continue;
	}
	if (deferred)
	  o->gch.gct = 0;
	continue;
      }
      if (!gc_chain_splice(p, o)) {
	gc_root_wait_no_l();
	continue;
      }
      gc_freefunc[o->gch.gct - ~LJ_TSTR](g, o);
      if (deferred)
	o->gch.gct = 0;  /* Body is awaiting bitmap reuse; destructor is done. */
    }
  }
  return p;
}

/* Sweep one string interning table chain. Preserves hashalg bit. */
static void gc_sweepstr(global_State *g, GCRef *chain)
{
  /* Mask with other white and LJ_GC_FIXED. Or LJ_GC_SFIXED on shutdown. */
  int ow = otherwhite(g);
  uintptr_t u = lj_str_ref_load_acq(chain);
  GCRef q;
  GCRef *p = &q;
  GCobj *o;
  lj_str_ref_store_rel(&q, u & ~(uintptr_t)LJ_STRHASH_LINKMASK);
  while ((o = gcref_acq(*p)) != NULL) {
    if (LJ_UNLIKELY(la_load8_acq(&o->gch.gct) != (uint8_t)~LJ_TSTR)) {
      /*
      ** String destructors stamp gct=0 before physical free. If a stale bucket
      ** link survives a lock-free resize/freeall race, unlink it from the
      ** chain snapshot instead of treating the old body as another live string.
      */
      lj_str_ref_store_rel(p, (uintptr_t)lj_str_next_acq(o));
      continue;
    }
    if (((lj_obj_gcflags(o) ^ LJ_GC_WHITES) & ow)) {  /* Black or current white? */
      lj_assertG(!isdead(g, o) || (lj_obj_gcflags(o) & LJ_GC_FIXED),
		 "sweep of undead string");
      makewhite(g, o);  /* String is alive, change to the current white. */
      p = lj_obj_gcwref(o);
    } else {  /* Otherwise string is dead, free it. */
      lj_assertG(isdead(g, o) || ow == LJ_GC_SFIXED,
		 "sweep of unlive string");
      lj_str_ref_store_rel(p, (uintptr_t)lj_str_next_acq(o));
      lj_str_free(g, gco2str(o));
    }
  }
  lj_str_ref_store_rel(chain,
		       lj_str_ref_load_acq(&q) | (u & LJ_STRHASH_SECONDARY));
}

/* Check whether we can clear a key or a value slot from a table. */
static int gc_mayclear(global_State *g, cTValue *o, int val)
{
  if (tvisgcv(o)) {  /* Only collectable objects can be weak references. */
    if (tvisstr(o)) {  /* But strings cannot be used as weak references. */
      gc_mark_str(g, strV(o));  /* And need to be marked. */
      return 0;
    }
    if (iswhite(gcV(o)))
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
      MSize i, asize = lj_tab_array_snapshot_acq(t, &array);
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
	if (gc_mayclear(g, &val, 1))
	  lj_tab_storenilraw(slot);
      }
    }
    {
      MSize i, hmask;
      Node *node = lj_tab_node_snapshot_acq(t, &hmask);
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
	  if (gc_mayclear(g, &key, 0) || gc_mayclear(g, &val, 1))
	    lj_tab_storenilraw(slot);
	}
      }
    }
    o = lj_tab_gclist_acq(t);
  }
}

#if LJ_HASFFI
static void gc_mark_finreg_tv(global_State *g, cTValue *tv)
{
  gc_marktv(g, tv);
}
#endif

/* Free all remaining GC objects. */
void lj_gc_freeall(global_State *g)
{
  MSize i;
  StrTabHdr *hdr;
  (void)lj_gc_flush_root_pending(g);
  /* Free everything, except super-fixed objects (the main thread). */
  g->gc.currentwhite = LJ_GC_WHITES | LJ_GC_SFIXED;
  gc_fullsweep(g, lj_gc_root_ref(g));
  hdr = lj_str_tabh_acq(g);
  if (hdr)
    for (i = hdr->mask; i != ~(MSize)0; i--)  /* Free all string hash chains. */
      gc_sweepstr(g, &hdr->bucket[i]);
  (void)lj_gc_sweep_gc2_all_arena_bodies(g);
}

/* -- Collector ----------------------------------------------------------- */

/* Atomic part of the GC cycle, transitioning from mark to sweep phase. */
static int atomic(global_State *g, lua_State *L)
{
  size_t udsize;

  gc_mark_uv(g);  /* Need to remark open upvalues (the thread may be dead). */
  gc_propagate_gray(g);  /* Propagate any left-overs. */

  lj_gc_list_move_rel(&g->gc.gray, &g->gc.weak);  /* Empty weak tables. */
  lj_assertG(!iswhite(obj2gco(mainthread_acq(g))), "main thread turned white");
  gc_markobj(g, L);  /* Mark running thread. */
  gc_traverse_curtrace(g);  /* Traverse current trace. */
  gc_mark_gcroot(g);  /* Mark GC roots (again). */
  gc_propagate_gray(g);  /* Propagate all of the above. */

  lj_gc_list_move_rel(&g->gc.gray, &g->gc.grayagain);  /* Empty 2nd chance. */
  gc_propagate_gray(g);  /* Propagate it. */

  /* 05 section 5.7.1 classic-GC atomic fixpoint-round bridge. */
  if (!gc2_legacy_mark_complete(g, L))
    return 0;

  /* Separate userdata to be finalized. */
  udsize = lj_gc2_finreg_udata_finalize(g, 0);
  gc_mark_finalizers(g);  /* Mark them. */
  udsize += gc_propagate_gray(g);  /* And propagate the marks. */
  /* 05 section 5.7.1 classic-GC atomic fixpoint-round bridge. */
  if (!gc2_legacy_mark_complete(g, L))
    return 0;
  gc2_mark_legacy_live_root_spine(g);
  gc2_paranoia_check_fixpoint(g);
  if (lj_tg_any_jit_active(g))
    return 0;

  /* All marking done, clear weak tables. */
  lj_gc2_mark_to_weak(g);
#if LJ_HASFFI
  if (lj_gc2_finreg_cdata_finalize_pweak(L, g, gc_mark_finreg_tv))
    (void)gc_propagate_gray(g);
#endif
  {
    GCobj *weak = lj_gc_list_head_acq(&g->gc.weak);
    if (!lj_gc2_weak_complete(g, L, weak, LJ_GC2_WEAK_DRAIN_BATCH)) {
      if (lj_tg_any_jit_active(g))
	return 0;
      lj_gc_clearweak_bridge(g, weak);
    }
  }
  if (lj_tg_any_jit_active(g))
    return 0;
  lj_gc2_weak_to_sweep(g, L);
  if (gc2_phase_acq(g) == LJ_GC2_WEAK)
    return 0;

  lj_buf_shrink(L, &G2TG(g)->tmpbuf);  /* Shrink temp buffer. */

  /* Prepare for sweep phase. */
  (void)lj_gc_flush_root_pending(g);
  g->gc.currentwhite = (uint8_t)otherwhite(g);  /* Flip current white. */
  g->strempty.marked = g->gc.currentwhite;
  setmref(g->gc.sweep, lj_gc_root_ref(g));
  g->gc.estimate = lj_gc_total_load(g) - (GCSize)udsize;  /* Initial estimate. */
  return 1;
}

#if LJ_HASJIT
int lj_gc_jit_defer_fixpoint(global_State *g)
{
  return lj_tg_any_jit_active(g) &&
	 g->gc.state == GCSpropagate &&
	 lj_gc_list_head_acq(&g->gc.gray) == NULL &&
	 lj_gc2_mark_phase_active(g);
}
#else
#define lj_gc_jit_defer_fixpoint(g)	0
#endif

/* GC state machine. Returns a cost estimate for each step performed. */
static size_t gc_onestep(lua_State *L)
{
  global_State *g = G(L);
  (void)lj_safepoint_poll(L);  /* Let worker-led handshakes finish between GC steps. */
  switch (g->gc.state) {
  case GCSpause:
    gc_mark_start(g);  /* Start a new GC cycle by marking all GC roots. */
    return 0;
  case GCSpropagate:
    if (lj_gc_list_head_acq(&g->gc.gray) != NULL)
      return propagatemark(g);  /* Propagate one gray object. */
    if (lj_gc2_worker_drain(g, LJ_GC2_WORKER_DRAIN_BATCH) != 0)
      return GCSWEEPCOST;  /* 05 section 5.6.3 bounded worker step bridge. */
    if (lj_gc2_mark_phase_active(g)) {
      if (lj_gc_jit_defer_fixpoint(g))
	return LJ_MAX_MEM;  /* Root handshakes are run after trace exit. */
      if (lj_gc2_fixpoint_round(g, L, LJ_GC2_WORKER_DRAIN_BATCH) == 0)
	return GCSWEEPCOST;  /* 05 section 5.7.1 bounded propagation fixpoint bridge. */
    }
    g->gc.state = GCSatomic;  /* End of mark phase. */
    return 0;
  case GCSatomic:
    if (lj_tg_any_jit_active(g))  /* Don't run atomic phase on trace. */
      return LJ_MAX_MEM;
    if (!atomic(g, L))
      return GCSWEEPCOST;
    g->gc.state = GCSsweepstring;  /* Start of sweep phase. */
    g->gc.sweepstr = 0;
    return 0;
  case GCSsweepstring: {
    GCSize old = lj_gc_total_load(g);
    StrTabHdr *hdr = lj_str_tabh_acq(g);
    if (hdr && g->gc.sweepstr <= hdr->mask) {
      if (lj_str_sweep_claim(L, hdr)) {
	gc_sweepstr(g, &hdr->bucket[g->gc.sweepstr]);  /* Sweep one chain. */
	g->gc.sweepstr++;
	lj_str_sweep_release(hdr);
      }
    }
    if (!hdr || g->gc.sweepstr > hdr->mask)
      g->gc.state = GCSsweep;  /* All string hash chains sweeped. */
    {
      GCSize total = lj_gc_total_load(g);
      /*
      ** Stock LuaJIT sweeps with a single mutator and can assert that sweeping
      ** only decreases total bytes. Here other TGs may allocate while this TG
      ** owns one sweep step, so fold either delta direction into the live-size
      ** estimate used for pacing.
      */
      if (old >= total)
	g->gc.estimate -= old - total;
      else
	g->gc.estimate += total - old;
    }
    return GCSWEEPCOST;
    }
  case GCSsweep: {
    GCSize old = lj_gc_total_load(g);
    setmref(g->gc.sweep, gc_sweep(g, mref(g->gc.sweep, GCRef), GCSWEEPMAX));
    {
      GCSize total = lj_gc_total_load(g);
      if (old >= total)
	g->gc.estimate -= old - total;
      else
	g->gc.estimate += total - old;
    }
    if (gcref_acq(*mref(g->gc.sweep, GCRef)) == NULL) {
      int arena_prepare = gc_arena_sweep_needs_prepare(g);
      if (!gc_arena_sweep_pending(g) || arena_prepare) {
	StrTabHdr *hdr = lj_str_tabh_acq(g);
	MSize mask = hdr ? hdr->mask : ~(MSize)0;
	if (la_load32_acq(&g->str.num) <= (mask >> 2) &&
	    mask > LJ_MIN_STRTAB*2-1)
	  lj_str_resize(L, mask >> 1);  /* Shrink string table. */
      }
      if (arena_prepare && lj_gc2_sweep_minor_active(g))
	(void)lj_gc_sweep_gc2_unmarked(g);
      if (arena_prepare)
	gc_arena_verify_sweep_boundary(g);
      (void)gc_arena_finish_sweep_boundary(g, 0);
      lj_gc2_sweep_bridge_boundary_reached(g);
      if (gc_arena_sweep_pending(g))
	return GCSWEEPMAX*GCSWEEPCOST;
      if (lj_gc2_finalizer_phase_pending(g)) {  /* Need finalizations? */
	g->gc.state = GCSfinalize;
      } else {  /* Otherwise skip this phase to help the JIT. */
	if (lj_gc2_sweep_bridge_close(g)) {
	  g->gc.state = GCSpause;  /* End of GC cycle. */
	  g->gc.debt = 0;
	} else {
	  g->gc.state = GCSsweep;
	}
      }
    }
    return GCSWEEPMAX*GCSWEEPCOST;
    }
  case GCSfinalize:
    {
      GCSize fincost;
      int finstep = lj_gc2_finalizer_step(L, GCFINALIZECOST, &fincost);
      if (finstep != 0)
	return fincost;
    }
    (void)gc_arena_finish_sweep_boundary(g, 1);
    if (lj_gc2_sweep_bridge_close(g)) {
      g->gc.state = GCSpause;  /* End of GC cycle. */
      g->gc.debt = 0;
    }
    return 0;
  default:
    lj_assertG(0, "bad GC state");
    return 0;
  }
}

static int gc_step_limited(lua_State *L, GCSize quantum, int batch_threshold)
{
  global_State *g = G(L);
  GCSize lim;
  int32_t ostate = vmstate_load_acq(g);
  setvmstate(g, GC);
  lim = (GCSTEPSIZE/100) * lj_gc_stepmul_load(g);
  if (lim == 0)
    lim = LJ_MAX_MEM;
  {
    GCSize threshold = lj_gc_threshold_load(g);
    GCSize total = lj_gc_total_load(g);
    if (total > threshold)
      g->gc.debt += total - threshold;
  }
  do {
    lim -= (GCSize)gc_onestep(L);
    if (g->gc.state == GCSpause) {
      lj_gc2_publish_idle_threshold(g);
      vmstate_store_rel(g, ostate);
      return 1;  /* Finished a GC cycle. */
    }
    if (batch_threshold && gc2_phase_acq(g) != LJ_GC2_IDLE)
      break;
  } while (sizeof(lim) == 8 ? ((int64_t)lim > 0) : ((int32_t)lim > 0));
  if (batch_threshold && gc2_phase_acq(g) != LJ_GC2_IDLE) {
    /*
    ** Automatic allocation checks must make one bounded GC2 state-machine step
    ** without carrying classic single-thread debt through an active concurrent
    ** phase. Otherwise trace-side helpers repeatedly catch up that debt by
    ** draining GC2 work on the mutator. Public collectgarbage("step") uses the
    ** explicit path and keeps the stock debt accounting contract.
    */
    g->gc.debt = 0;
    lj_gc_threshold_store(g, lj_gc_total_load(g) + quantum);
    vmstate_store_rel(g, ostate);
    return -1;
  }
  {
    if (g->gc.debt < quantum) {
      lj_gc_threshold_store(g, lj_gc_total_load(g) + quantum);
      vmstate_store_rel(g, ostate);
      return -1;
    }
    g->gc.debt -= quantum;
    lj_gc_threshold_store(g, lj_gc_total_load(g) +
			  (batch_threshold ? quantum : 0));
    vmstate_store_rel(g, ostate);
    return 0;
  }
}

/* Perform a limited amount of incremental GC steps. */
int LJ_FASTCALL lj_gc_step(lua_State *L)
{
  return gc_step_limited(L, gc_step_debt_quantum(G(L)), 1);
}

int lj_gc_step_explicit(lua_State *L)
{
  return gc_step_limited(L, GCSTEPSIZE, 0);
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
    lj_gc_step(L);
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
  int threshold_step, hard_step;
  TGState *tg;
  if (!L || !jbase)
    return 1;
  L->base = jbase;
  L->top = curr_topL(L);
  tg = L2TG(L);
  lj_gc2_check_trigger(g, tg);
  threshold_step = lj_gc_total_load(g) >= lj_gc_threshold_load(g);
  hard_step = gc_hard_assist_due_jit(g);
  if (hard_step) {
    gc2_jit_hard_checks_add(g, 1);
    lj_gc2_assist(g, tg);  /* 05 section 5.11 trace-side assist bridge. */
    lj_gc2_hard_check_advance(g, lj_gc2_alloc_since_load(g));
  }
  if (threshold_step) {
    while (steps-- > 0 && lj_gc_step(L) == 0)
      ;
  }
  /* Return 1 to force a trace exit. */
  return lj_gc_jit_defer_fixpoint(g) ||
	 (G(L)->gc.state == GCSatomic || G(L)->gc.state == GCSfinalize);
}
#endif

/* Perform a full GC cycle. */
void lj_gc_fullgc(lua_State *L)
{
  global_State *g = G(L);
  int32_t ostate = vmstate_load_acq(g);
  setvmstate(g, GC);
  (void)lj_gc_flush_root_pending(g);
  while (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
    /* A forced full collection may arrive while a concurrent/scoped JIT flush
    ** has left additional roots on the gray lists. Finish the mark fixpoint
    ** before sweeping; clearing partial propagation state would preserve a
    ** table body while letting its key/value objects be collected.
    */
    gc_onestep(L);
    if (lj_gc2_finalizer_fullgc_deferred(g)) {
      vmstate_store_rel(g, ostate);
      return;
    }
  }
  while (g->gc.state == GCSsweepstring || g->gc.state == GCSsweep) {
    gc_arena_fullgc_drain_sweep(g);
    gc_onestep(L);  /* Finish sweep. */
  }
  lj_assertG(g->gc.state == GCSfinalize || g->gc.state == GCSpause,
	     "bad GC state");
  /* Now perform a full GC. */
  lj_gc2_force_major(g);
  g->gc.state = GCSpause;
  do {
    gc_arena_fullgc_drain_sweep(g);
    gc_onestep(L);
    if (lj_gc2_finalizer_fullgc_deferred(g)) {
      vmstate_store_rel(g, ostate);
      return;
    }
  } while (g->gc.state != GCSpause);
  /*
  ** Full collection may shrink lockless side tables during the final sweep.
  ** Those old headers are SMR-retired, so advance one grace epoch here instead
  ** of making callers wait for an unrelated later safepoint to reclaim them.
  */
  (void)lj_gc2_handshake(g, LJ_GC2_HS_FLUSH_SSB);
  lj_gc2_publish_idle_threshold(g);
  vmstate_store_rel(g, ostate);
}

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
    GCobj *o = gcV(&snap);
    if (isdead(g, o)) {
      makewhite(g, o);
      (void)lj_gc2_markobj_nolegacy(g, o);
      gc_mark(g, o);
      (void)gc_propagate_gray(g);
      return;
    }
    if (iswhite(o) && (g->gc.state == GCSpropagate ||
		       g->gc.state == GCSatomic)) {
      (void)lj_gc2_markobj_nolegacy(g, o);
      gc_mark(g, o);
      return;
    }
    lj_gc2_barrier_tv_g(g, &snap);
  }
}

/* Publish a GC object that is becoming reachable from a native root list. */
void lj_gc_pubobjroot(lua_State *L, GCobj *o)
{
  TValue tv;
  if (!L || !o || LJ_UNLIKELY(o->gch.gct == 0))
    return;
  setgcV(L, &tv, o, ~o->gch.gct);
  lj_gc_pubroot(L, &tv);
}

/* Move the GC propagation frontier forward. */
void lj_gc_barrierf(global_State *g, GCobj *o, GCobj *v)
{
  lj_assertG(isblack(o) && iswhite(v) && !isdead(g, v) && !isdead(g, o),
	     "bad object states for forward barrier");
  lj_assertG(g->gc.state != GCSfinalize && g->gc.state != GCSpause,
	     "bad GC state");
  lj_assertG(o->gch.gct != ~LJ_TTAB, "barrier object is not a table");
  /* Preserve invariant during propagation. Otherwise it doesn't matter. */
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
    (void)lj_gc2_markobj_nolegacy(g, v);
    gc_mark(g, v);  /* Move frontier forward. */
  } else {
    makewhite(g, o);  /* Make it white to avoid the following barrier. */
  }
}

/* VM-callable table black-to-gray repair. */
void lj_gc_barrierback_tab_g(global_State *g, GCtab *t)
{
  if (g && t && isblack(obj2gco(t)))
    lj_gc_barrierback(g, t);
}

/* Publication wrapper for x64 VM table -> object stores. */
void lj_gc_pubtabobj_vm(lua_State *L, GCtab *t, GCobj *o)
{
  int white;
  if (!L || !t || !o)
    return;
  white = iswhite(o);
  lj_gc2_barrier_obj_pair(L, obj2gco(t), o);
  if (white && isblack(obj2gco(t)))
    lj_gc_barrierback(G(L), t);
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
  uint32_t i;
  if (!L || !t || !tv || n == 0)
    return;
  g = G(L);
  lj_gc2_barrier_tvn_pair_g(g, obj2gco(t), tv, n);
  lj_gc2_barrier_tab(L, t);  /* Preserve the previous TSETM table barrier. */
  if (!isblack(obj2gco(t)))
    return;
  for (i = 0; i < n; i++) {
    TValue snap;
    lj_tv_load_acq(&snap, &tv[i]);
    if (tviswhite(&snap)) {
      lj_gc_barrierback(g, t);
      return;
    }
  }
}

/* Publication wrapper for closed-upvalue TValue stores. Pass &uv->tv. */
void LJ_FASTCALL lj_gc_pubuv(global_State *g, TValue *tv)
{
#define TV2MARKED(x) \
  (*((uint8_t *)(x) - offsetof(GCupval, tv) + offsetof(GCupval, marked)))
  GCupval *uv = (GCupval *)((char *)tv - offsetof(GCupval, tv));
  TValue snap;
  int white;
  lj_tv_load_acq(&snap, tv);
  if (!tvisgcv(&snap))
    return;
  white = tviswhite(&snap);
  lj_gc2_barrier_tv_pair_g(g, obj2gco(uv), &snap);
  if ((TV2MARKED(tv) & LJ_GC_BLACK) && white) {
    if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
      (void)lj_gc2_markobj_nolegacy(g, gcV(&snap));
      gc_mark(g, gcV(&snap));
    } else {
      TV2MARKED(tv) = (TV2MARKED(tv) & (uint8_t)~LJ_GC_COLORS) | curwhite(g);
    }
  }
#undef TV2MARKED
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
  if (isgray(o)) {  /* A closed upvalue is never gray, so fix this. */
    if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
      TValue tv;
      gray2black(o);  /* Make it black and preserve invariant. */
      lj_tv_load_acq(&tv, &uv->tv);
      if (tviswhite(&tv))
	lj_gc_barrierf(g, o, gcV(&tv));
    } else {
      makewhite(g, o);  /* Make it white, i.e. sweep the upvalue. */
      lj_assertG(g->gc.state != GCSfinalize && g->gc.state != GCSpause,
		 "bad GC state");
    }
  }
}

#if LJ_HASJIT
/* Mark a trace if it's saved during the propagation phase. */
void lj_gc_pubtrace(global_State *g, uint32_t traceno)
{
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic)
    gc_marktrace(g, traceno);
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

/* Allocate raw storage for a GC object without linking it. */
void *lj_mem_newgco_raw(lua_State *L, GCSize size, uint32_t flags)
{
  global_State *g = G(L);
  GCobj *o;
  if (g->allocf == lj_arena_allocf)
    o = (GCobj *)lj_arena_allocd_alloc(gc_arena_allocd_for_new(L), size,
				       flags);
  else
    o = (GCobj *)g->allocf(g->allocd, NULL, 0, size);
  if (o == NULL)
    lj_err_mem(L);
  lj_assertG(checkptrGC(o),
	     "allocated memory address %p outside required range", o);
  lj_gc_total_add(g, size);
  lj_gc2_account_alloc(g, L2TG(L), size);  /* 04 section 4.8. */
  return o;
}

void *lj_mem_newgco_unlinked(lua_State *L, GCSize size)
{
  return lj_mem_newgco_raw(L, size, LJ_AF_TRAVERSABLE);
}

void lj_gc_linkobj(global_State *g, GCobj *o)
{
#if LJ_GC64
  uint64_t head;
  GCRef next;
  do {
    head = la_load64_acq(&lj_gc_root_ref(g)->gcptr64);
    setgcrefp(next, (void *)(uintptr_t)head);
    lj_obj_setgcwr(o, next);
  } while (!la_cas64(&lj_gc_root_ref(g)->gcptr64, &head,
		     (uint64_t)(uintptr_t)&o->gch, LA_REL, LA_ACQ));  /* M7 publish. */
#else
  uint32_t head;
  GCRef next;
  do {
    head = la_load32_acq(&lj_gc_root_ref(g)->gcptr32);
    setgcrefp(next, (void *)(uintptr_t)head);
    lj_obj_setgcwr(o, next);
  } while (!la_cas32(&lj_gc_root_ref(g)->gcptr32, &head,
		     (uint32_t)(uintptr_t)&o->gch, LA_REL, LA_ACQ));  /* M7 publish. */
#endif
}

static int gc_root_chain_break_cycle(GCobj *head)
{
  GCobj *slow = head, *fast = head, *entry, *tail;
  while (fast != NULL) {
    slow = lj_obj_gcw_acq(slow);
    fast = lj_obj_gcw_acq(fast);
    if (fast == NULL || slow == NULL)
      return 0;
    fast = lj_obj_gcw_acq(fast);
    if (slow == fast)
      break;
  }
  if (fast == NULL)
    return 0;
  slow = head;
  while (slow != fast) {
    slow = lj_obj_gcw_acq(slow);
    fast = lj_obj_gcw_acq(fast);
  }
  entry = slow;
  tail = entry;
  /*
  ** Pending-root chains are caller-owned stacks and must be null-terminated
  ** before they are spliced into the legacy root spine. If a racing publisher or
  ** an old corrupted pending head forms a cycle, preserve each unique object in
  ** the chain by severing the cycle predecessor rather than letting GC hang.
  */
  while (lj_obj_gcw_acq(tail) != entry)
    tail = lj_obj_gcw_acq(tail);
  lj_obj_setgcwnullrel(tail);
  return 1;
}

static uint32_t gc_root_chain_tail(GCobj *head, GCobj **tailp)
{
  GCobj *tail, *next;
  uint32_t n = 0;
  if (!head) {
    *tailp = NULL;
    return 0;
  }
  (void)gc_root_chain_break_cycle(head);
  tail = head;
  do {
    if (n != ~(uint32_t)0)
      n++;
    next = lj_obj_gcw_acq(tail);
    if (!next)
      break;
    tail = next;
  } while (1);
  *tailp = tail;
  return n;
}

static uint32_t gc_root_prepend_chain(global_State *g, GCobj *head)
{
  GCobj *tail;
  uint32_t n = gc_root_chain_tail(head, &tail);
  if (!n)
    return 0;
#if LJ_GC64
  {
    uint64_t oldhead;
    GCRef nextref;
    do {
      oldhead = la_load64_acq(&lj_gc_root_ref(g)->gcptr64);
      setgcrefp(nextref, (void *)(uintptr_t)oldhead);
      lj_obj_setgcwrrel(tail, nextref);
    } while (!la_cas64(&lj_gc_root_ref(g)->gcptr64, &oldhead,
		       (uint64_t)(uintptr_t)&head->gch, LA_REL, LA_ACQ));
  }
#else
  {
    uint32_t oldhead;
    GCRef nextref;
    do {
      oldhead = la_load32_acq(&lj_gc_root_ref(g)->gcptr32);
      setgcrefp(nextref, (void *)(uintptr_t)oldhead);
      lj_obj_setgcwrrel(tail, nextref);
    } while (!la_cas32(&lj_gc_root_ref(g)->gcptr32, &oldhead,
		       (uint32_t)(uintptr_t)&head->gch, LA_REL, LA_ACQ));
  }
#endif
  return n;
}

static void gc_root_prepend_known_chain(global_State *g, GCobj *head,
					GCobj *tail)
{
#if LJ_GC64
  {
    uint64_t oldhead;
    GCRef nextref;
    do {
      oldhead = la_load64_acq(&lj_gc_root_ref(g)->gcptr64);
      setgcrefp(nextref, (void *)(uintptr_t)oldhead);
      lj_obj_setgcwrrel(tail, nextref);
    } while (!la_cas64(&lj_gc_root_ref(g)->gcptr64, &oldhead,
		       (uint64_t)(uintptr_t)&head->gch, LA_REL, LA_ACQ));
  }
#else
  {
    uint32_t oldhead;
    GCRef nextref;
    do {
      oldhead = la_load32_acq(&lj_gc_root_ref(g)->gcptr32);
      setgcrefp(nextref, (void *)(uintptr_t)oldhead);
      lj_obj_setgcwrrel(tail, nextref);
    } while (!la_cas32(&lj_gc_root_ref(g)->gcptr32, &oldhead,
		       (uint32_t)(uintptr_t)&head->gch, LA_REL, LA_ACQ));
  }
#endif
}

static uint32_t gc_root_prepend_chain_after(GCobj *anchor, GCobj *head)
{
  GCRef *p;
  GCobj *tail, *oldhead;
  uint32_t n = gc_root_chain_tail(head, &tail);
  if (!anchor || !n)
    return 0;
  p = lj_obj_gcwref(anchor);
#if LJ_GC64
  {
    uint64_t expect;
    do {
      oldhead = gcref_acq(*p);
      if (oldhead)
	lj_obj_setgcwrel(tail, oldhead);
      else
	lj_obj_setgcwnullrel(tail);
      expect = oldhead ? (uint64_t)(uintptr_t)&oldhead->gch : 0;
    } while (!la_cas64(&p->gcptr64, &expect,
		       (uint64_t)(uintptr_t)&head->gch,
		       LA_REL, LA_ACQ));
  }
#else
  {
    uint32_t expect;
    do {
      oldhead = gcref_acq(*p);
      if (oldhead)
	lj_obj_setgcwrel(tail, oldhead);
      else
	lj_obj_setgcwnullrel(tail);
      expect = oldhead ? (uint32_t)(uintptr_t)&oldhead->gch : 0;
    } while (!la_cas32(&p->gcptr32, &expect,
		       (uint32_t)(uintptr_t)&head->gch,
		       LA_REL, LA_ACQ));
  }
#endif
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
  n += gc_root_prepend_chain_after(obj2gco(mainthread_acq(g)), head);
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
    if (head)
      lj_obj_setgcwrel(o, head);
    else
      lj_obj_setgcwnullrel(o);
    lj_tg_gcroot_pending_store_transition_rel(tg, head, o);
    return;
  }
  head = lj_tg_gcroot_pending_acq(tg);
  do {
    if (head)
      lj_obj_setgcwrel(o, head);
    else
      lj_obj_setgcwnullrel(o);
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
    if (oldhead)
      lj_obj_setgcwrel(tail, oldhead);
    else
      lj_obj_setgcwnullrel(tail);
    lj_tg_gcroot_pending_store_transition_rel(tg, oldhead, head);
    return;
  }
  oldhead = lj_tg_gcroot_pending_acq(tg);
  do {
    if (oldhead)
      lj_obj_setgcwrel(tail, oldhead);
    else
      lj_obj_setgcwnullrel(tail);
  } while (!lj_tg_gcroot_pending_cas(tg, &oldhead, head));
}

void lj_gc_linkobj_new_after_main(global_State *g, GCobj *o)
{
  TGState *tg = lj_thr_get_tg();
  GCobj *head;
  if (!tg || tg->gl != g || lj_tg_flags_test_acq(tg, TGF_DEAD)) {
    lj_gc_linkobj_after(obj2gco(mainthread_acq(g)), o);
    return;
  }
  if (LJ_LIKELY(tg == g->main_tg && mt_active_acq(g) == 0 &&
		mt_entering_acq(g) == 0 && gc2_n_workers_acq(g) == 0)) {
    head = lj_tg_gcroot_pending_after_main_acq(tg);
    if (head)
      lj_obj_setgcwrel(o, head);
    else
      lj_obj_setgcwnullrel(o);
    lj_tg_gcroot_pending_after_main_store_transition_rel(tg, head, o);
    return;
  }
  head = lj_tg_gcroot_pending_after_main_acq(tg);
  do {
    if (head)
      lj_obj_setgcwrel(o, head);
    else
      lj_obj_setgcwnullrel(o);
  } while (!lj_tg_gcroot_pending_after_main_cas(tg, &head, o));
}

void lj_gc_linkobj_after(GCobj *anchor, GCobj *o)
{
  GCRef *p;
  GCobj *head;
  if (!anchor || !o)
    return;
  p = lj_obj_gcwref(anchor);
#if LJ_GC64
  {
    uint64_t expect;
    do {
      head = gcref_acq(*p);
      if (head)
	lj_obj_setgcw(o, head);
      else
	lj_obj_setgcwnull(o);
      expect = head ? (uint64_t)(uintptr_t)&head->gch : 0;
    } while (!la_cas64(&p->gcptr64, &expect,
		       (uint64_t)(uintptr_t)&o->gch,
		       LA_REL, LA_ACQ));
  }
#else
  {
    uint32_t expect;
    do {
      head = gcref_acq(*p);
      if (head)
	lj_obj_setgcw(o, head);
      else
	lj_obj_setgcwnull(o);
      expect = head ? (uint32_t)(uintptr_t)&head->gch : 0;
    } while (!la_cas32(&p->gcptr32, &expect,
		       (uint32_t)(uintptr_t)&o->gch,
		       LA_REL, LA_ACQ));
  }
#endif
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
** the body is only waiting for bitmap reuse. Stamp gct=0 here as well as in
** the legacy sweep callers so arena scans do not mistake stale header bytes in
** reused cells for a live object that still needs a destructor.
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
  lj_gc_total_sub(g, osize);
  ((GCobj *)p)->gch.gct = 0;
  return 1;
}
