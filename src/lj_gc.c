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
#include "lj_dispatch.h"
#include "lj_arena.h"
#include "lj_vm.h"
#include "lj_vmevent.h"

#define GCSTEPSIZE	1024u
#define GCSWEEPMAX	40
#define GCSWEEPCOST	10
#define GCFINALIZECOST	100

/* Macros to set GCobj colors and flags. */
#define white2gray(x)		(lj_obj_cleargcflags((x), LJ_GC_WHITES))
#define gray2black(x)		(lj_obj_addgcflags((x), LJ_GC_BLACK))
#define isfinalized(u)		(lj_obj_gcflags(obj2gco(u)) & LJ_GC_FINALIZED)

static void gc_root_wait_no_l(void)
{
  (void)lj_thr_sleep_ns(NULL, 1000000);
}

#if LJ_HASFFI
static void gc_finreg_claim_wait_no_l(void)
{
  gc_root_wait_no_l();
}
#endif

/* -- Mark phase ---------------------------------------------------------- */

static LJ_AINLINE int gc2_suppress_legacy_mark(global_State *g)
{
  return gc2_phase_acq(g) == LJ_GC2_MARK &&
	 gc2_cycle_roots_minor_acq(g) != 0;
}

void lj_gc_arena_markobj(global_State *g, GCobj *o)
{
  if (!gc2_suppress_legacy_mark(g))
    lj_gc2_markobj(g, o);
}

void lj_gc_arena_markmem(global_State *g, void *p)
{
  if (!gc2_suppress_legacy_mark(g))
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
  return gc2_phase_acq(g) == LJ_GC2_SWEEP &&
	 !lj_gc2_finalizer_sweep_pending(g);
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
  uint32_t n = 0;
  for (o = gcref_acq(g->gc.root); o != NULL; o = lj_obj_gcw_acq(o)) {
    lj_gc_arena_markobj(g, o);
    if (++n == 1000000u) {
      lj_assertG(0, "root list cycle at arena sweep boundary");
      break;
    }
  }
}

static uint32_t gc_arena_finish_sweep_boundary(global_State *g, int drain)
{
  TGState *tg;
  uint32_t total = 0;
  uint32_t cycle;
  if (!gc_arena_sweep_ready(g)) {
    gc_arena_rebuild_free(g);
    return 0;
  }
  cycle = gc2_cycle_acq(g);
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg)) {
    /* 05 section 5.8 boundary-lazy traversable sweep bridge. */
    if (lj_gc2_sweep_tg_ready(tg) &&
	tg->alloc.prepare_epoch != cycle) {
      lj_arena_alloc_prepare_sweep_kind(&tg->alloc, LJ_ARENAK_TRAVERSABLE);
      lj_arena_alloc_restore_sweep_kind(&tg->alloc, LJ_ARENAK_PLAIN);
      tg->alloc.prepare_epoch = cycle;
    }
  }
  if (!gc2_sweep_legacy_ready_acq(g)) {
    gc_arena_preserve_root_chain(g);
    lj_gc2_sweep_legacy_ready(g);
  }
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

static int gc2_legacy_sweep_close(global_State *g)
{
  if (gc2_phase_acq(g) == LJ_GC2_SWEEP)
    return lj_gc2_sweep_to_idle(g);
  lj_gc2_legacy_cycle_end(g);  /* Preserving full-GC fast-forward sweep. */
  return 1;
}

#ifdef LUA_USE_ASSERT
static void gc_arena_verify_marked(global_State *g, GCobj *o)
{
  TGState *tg = G2TG(g);
  int marked;
  if (!tg || !lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL))
    return;
  marked = lj_gc2_ismarked(g, o);
  if (marked < 0)
    return;  /* Custom aligned objects need allocation-base marking first. */
  lj_assertG(marked != 0, "unmarked arena object at verify boundary");
}

static void gc_arena_verify_sweep_boundary(global_State *g)
{
  TGState *tg = G2TG(g);
  GCobj *o;
  if (!tg || !lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL) ||
      gc2_phase_acq(g) != LJ_GC2_SWEEP ||
      lj_gc2_finalizer_sweep_pending(g))
    return;
  for (o = gcref_acq(g->gc.root); o != NULL; o = lj_obj_gcw_acq(o)) {
    gc_arena_verify_marked(g, o);
    if (o->gch.gct == ~LJ_TTHREAD) {
      GCobj *uv;
      for (uv = gcref_acq(gco2th(o)->openupval); uv != NULL;
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
  for (uv = gcref_acq(th->openupval); uv != NULL; uv = lj_obj_gcw_acq(uv))
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
  if (udtype == UDTYPE_FFI_PIN) {
    TValue tv;
    lj_tv_load_acq(&tv, (TValue *)uddata(ud));
    if (tvisgcv(&tv))
      gc2_paranoia_checkobj(g, gcV(&tv), "FFI pin value");
  }
#endif
#if LJ_HASFFI
  if (udtype == UDTYPE_FFI_CLIB) {
    CLibrary *cl = (CLibrary *)uddata(ud);
    CLibCacheEntry *e;
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
  for (o = gcref_acq(g->gc.root); o != NULL; o = lj_obj_gcw_acq(o))
    gc2_paranoia_checkone(g, o);
  lj_gc2_finalizer_mark_all(g, gc2_paranoia_check_finalizer_obj);
}

static void gc2_paranoia_check_rawroots(global_State *g)
{
  StrTabHdr *hdr;
  TabNodeRetire *ret;
  TabArrayRetire *aret;
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
    TGState *tg = G2TG(g);
    if (tg)
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
      {
	GCtab *pinmt = ctype_pinmt_acq(cts);
	if (pinmt)
	  gc2_paranoia_checkobj(g, obj2gco(pinmt), "FFI pin metatable");
      }
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
}

static void gc2_paranoia_check_fixpoint(global_State *g)
{
  if (gc2_cycle_roots_minor_acq(g))
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
  { lj_assertG(!tvisgcv(tv) || (~itype(tv) == gcval(tv)->gch.gct), \
	       "TValue and GC type mismatch"); \
    if (tvisgcv(tv)) { lj_gc_arena_markobj((g), gcV(tv)); \
      if (tviswhite(tv)) gc_mark((g), gcV(tv)); } }

/* Mark a GCobj (if needed). */
#define gc_markobj(g, o) \
  { lj_gc_arena_markobj((g), obj2gco(o)); \
    if (iswhite(obj2gco(o))) gc_mark((g), obj2gco(o)); }

/* Mark a string object. */
#define gc_mark_str(g, s) \
  (lj_gc_arena_markobj((g), obj2gco(s)), \
   lj_obj_cleargcflags(obj2gco(s), LJ_GC_WHITES))

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
  CLibCacheEntry *e;
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

/* Mark a white GCobj. */
static void gc_mark(global_State *g, GCobj *o)
{
  int gct = o->gch.gct;
  lj_assertG(iswhite(o), "mark of non-white object");
  lj_assertG(!isdead(g, o), "mark of dead object");
  lj_gc_arena_markobj(g, o);
  white2gray(o);
  if (LJ_UNLIKELY(gct == ~LJ_TUDATA)) {
    GCudata *ud = gco2ud(o);
    uint8_t udtype = lj_udata_udtype_acq(ud);
    GCtab *mt = tabref_acq(ud->metatable);
    GCtab *env = tabref_acq(ud->env);
    gray2black(o);  /* Userdata are never gray. */
    if (mt) gc_markobj(g, mt);
    if (env) gc_markobj(g, env);
#if LJ_HASFFI
    if (udtype == UDTYPE_FFI_CLIB) {
      CLibrary *cl = (CLibrary *)uddata(ud);
      gc_mark_clib_cache(g, cl);
    }
    if (udtype == UDTYPE_FFI_PIN) {
      TValue tv;
      lj_tv_load_acq(&tv, (TValue *)uddata(ud));
      gc_marktv(g, &tv);  /* 11.6 ffi.pin() root. */
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
      lua_State *child = lj_thread_state_load_acq(th);
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
	lj_udata_udtype_acq(gco2ud(o)) == UDTYPE_THREAD)
      gc_markobj(g, o);
  }
}

static void gc_mark_gcroot(global_State *g)
{
  ptrdiff_t i;
  for (i = 0; i < GCROOT_MAX; i++) {
    GCobj *o = gcref_acq(g->gcroot[i]);
    if (o != NULL)
      gc_markobj(g, o);
  }
  gc_mark_threading_live(g);
  gc_mark_fixedstr(g);
  gc_mark_strtab_mem(g);
  gc_mark_tab_retired_mem(g);
#if LJ_64
  lj_gc_arena_markmem(g, mref(g->gc.lightudseg, uint32_t));
#endif
  lj_gc_arena_markmem(g, g->tmpbuf.b);
  {
    TGState *tg = G2TG(g);
    if (tg)
      lj_gc_arena_markmem(g, tg->tmpbuf.b);
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
      {
	GCtab *pinmt = ctype_pinmt_acq(cts);
	if (pinmt)
	  gc_markobj(g, pinmt);
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
}

/* Start a GC cycle and mark the root set. */
static void gc_mark_start(global_State *g)
{
  lua_State *mainL = mainthread_acq(g);
  lua_State *vmL = vmthread_acq(g);
  lj_gc2_legacy_mark_begin(g);
  lj_gc_list_clear_rel(&g->gc.gray);
  lj_gc_list_clear_rel(&g->gc.grayagain);
  lj_gc_list_clear_rel(&g->gc.weak);
  gc_markobj(g, mainL);
  {
    GCtab *env = tabref_acq(mainL->env);
    if (env)
      gc_markobj(g, env);
  }
  gc_markobj(g, vmL);
  gc_marktv(g, &g->registrytv);
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

/* Separate userdata objects to be finalized to the GC2 finalizer queue. */
size_t lj_gc_separateudata(global_State *g, int all)
{
  return lj_gc2_finreg_udata_finalize(g, all);
}

/* -- Propagation phase --------------------------------------------------- */

static int gc_weak_list_has(global_State *g, GCtab *t)
{
  GCobj *want = obj2gco(t);
  GCobj *o;
  for (o = lj_gc_list_head_acq(&g->gc.weak); o != NULL;
       o = gcref_acq(gco2tab(o)->gclist))
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
  GCtab *mt = tabref_acq(t->metatable);
  arraymem = lj_tab_array_mem_acq(t);
  if (arraymem)
    lj_gc_arena_markmem(g, arraymem);
  {
    MSize hmask;
    Node *node = lj_tab_node_snapshot_acq(t, &hmask);
    if (hmask > 0)
      lj_gc_arena_markmem(g, lj_tab_node_hdrw(node));
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
	    gc_finreg_claim_wait_no_l();
	    lj_tv_load_acq(&val, &n->val);
	    lj_tv_load_acq(&key, &n->key);
	  }
	}
#endif
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
    GCtab *env = tabref_acq(fn->c.env);
    if (env)
      gc_markobj(g, env);
  }
  if (isluafunc(fn)) {
    uint32_t i;
    lj_assertG(fn->l.nupvalues <= funcproto(fn)->sizeuv,
	       "function upvalues out of range");
    gc_markobj(g, funcproto(fn));
    for (i = 0; i < fn->l.nupvalues; i++)  /* Mark Lua function upvalues. */
      gc_markobj(g, func_uv_acq(&fn->l, i));
  } else {
    uint32_t i;
    for (i = 0; i < fn->c.nupvalues; i++) {  /* Mark C function upvalues. */
      TValue tv;
      lj_tv_load_acq(&tv, &fn->c.upvalue[i]);
      gc_marktv(g, &tv);
    }
  }
}

#if LJ_HASJIT
/* Mark a trace. */
static void gc_marktrace(global_State *g, TraceNo traceno)
{
  GCtrace *T = traceref(G2J(g), traceno);
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
    if (!LJ_FR2) gc_markobj(g, fn);  /* Need to mark hidden function (or L). */
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

static void gc_mark_thread_root_tv(global_State *g, cTValue *tv)
{
  GCobj *o;
  if (!tvisgcv(tv))
    return;
  o = gcV(tv);
  lj_gc_arena_markobj(g, o);
  if (iswhite(o)) {
    gc_mark(g, o);
  } else if (tvistab(tv) && isblack(o)) {
    if (gc_traverse_tab(g, tabV(tv)) > 0)
      black2gray(o);
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
  else if (isblack(o)) {
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
  lj_gc_arena_markmem(g, tvref(th->stack));
  used = gc_traverse_frames(g, th);
  if (th == lj_tg_cur_L(g) && th->base > tvref(th->stack) + 1 + LJ_FR2) {
    top = gc_active_thread_top(th, top);
  } else if (tvref(th->stack) + used > top) {
    top = tvref(th->stack) + used;
  }
  for (o = tvref(th->stack)+1+LJ_FR2; o < top; o++) {
    lj_tv_load_acq(&tv, o);
    gc_mark_thread_root_tv(g, &tv);
  }
  if (g->gc.state == GCSatomic) {
    top = tvref(th->stack) + th->stacksize;
    for (; o < top; o++)  /* Clear unmarked slots. */
      setnilV(o);
  }
  for (mt = gcref_acq(th->openupval); mt != NULL; mt = lj_obj_gcw_acq(mt)) {
    lj_gc_arena_markobj(g, mt);
    if (iswhite(mt))
      gc_mark(g, mt);
  }
  {
    GCtab *env = tabref_acq(th->env);
    gc_mark_thread_root_tab(g, env);
  }
  mt = gcref_acq(th->mt_thread);
  if (mt != NULL)
    gc_markobj(g, mt);
  if (th != lj_tg_cur_L(g))
    lj_state_shrinkstack(th, used);
}

/* Propagate one gray object. Traverse it and turn it black. */
static size_t propagatemark(global_State *g)
{
  GCobj *o = lj_gc_list_head_acq(&g->gc.gray);
  int gct = o->gch.gct;
  lj_assertG(isgray(o), "propagation of non-gray object");
  gray2black(o);
  lj_gc_list_pop_head_rel(&g->gc.gray, o);  /* Remove from gray list. */
  if (LJ_LIKELY(gct == ~LJ_TTAB)) {
    GCtab *t = gco2tab(o);
    MSize acap = lj_tab_array_separated_acap_acq(t);
    MSize hmask;
    (void)lj_tab_node_snapshot_acq(t, &hmask);
    if (gc_traverse_tab(g, t) > 0)
      black2gray(o);  /* Keep weak tables gray. */
    return (LJ_MAX_COLOSIZE != 0 && t->colo ?
	    sizetabcolo((uint32_t)t->colo & 0x7f) : sizeof(GCtab)) +
	   (acap ? lj_tab_array_bytes(acap) : 0) +
	   (hmask ? lj_tab_node_bytes(hmask) : 0);
  } else if (LJ_LIKELY(gct == ~LJ_TFUNC)) {
    GCfunc *fn = gco2func(o);
    gc_traverse_func(g, fn);
    return isluafunc(fn) ? sizeLfunc((MSize)fn->l.nupvalues) :
			   sizeCfunc((MSize)fn->c.nupvalues);
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

static int gc2_valid_freeable_obj(GCobj *o)
{
  uint32_t gct = o->gch.gct;
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

static void gc2_unlink_root_obj(global_State *g, GCobj *dead)
{
  GCRef *p = &g->gc.root;
  GCobj *o;
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
  GCRef *p = &g->gc.root;
  GCobj *o;
  uint32_t n = 0;
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
      if (isdead(g, o) && gc2_valid_freeable_obj(o)) {
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
      if (unmarked_only && gc2_valid_freeable_obj(o)) {
	lj_arena_bm_set(a->mark, i);
	continue;
      }
      if ((!unmarked_only || isdead(g, o)) && gc2_valid_freeable_obj(o)) {
	gc2_unlink_root_obj(g, o);
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
  while ((o = gcref_acq(*p)) != NULL && lim-- > 0) {
    if (LJ_UNLIKELY(o->gch.gct == 0)) {
      if (!gc_chain_splice(p, o)) {
	gc_root_wait_no_l();
	continue;
      }
      continue;  /* Body destructor already ran via GC2 arena sweep. */
    }
    if (o->gch.gct == ~LJ_TTHREAD)  /* Need to sweep open upvalues, too. */
      gc_fullsweep(g, &gco2th(o)->openupval);
    if (((lj_obj_gcflags(o) ^ LJ_GC_WHITES) & ow)) {  /* Black or current white? */
      lj_assertG(!isdead(g, o) || (lj_obj_gcflags(o) & LJ_GC_FIXED),
		 "sweep of undead object");
      makewhite(g, o);  /* Value is alive, change to the current white. */
      p = lj_obj_gcwref(o);
    } else {  /* Otherwise value is dead, free it. */
      int deferred = gc2_deferred_body_pending(g, o);
      lj_assertG(isdead(g, o) || ow == LJ_GC_SFIXED,
		 "sweep of unlive object");
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
void lj_gc_clearweak_legacy(global_State *g, GCobj *o)
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
	lj_tv_load_acq(&val, &array[i]);
	if (gc_mayclear(g, &val, 1))
	  lj_tab_storenilraw(&array[i]);
      }
    }
    {
      MSize i, hmask;
      Node *node = lj_tab_node_snapshot_acq(t, &hmask);
      for (i = 0; i <= hmask; i++) {
	Node *n = &node[i];
	TValue key, val;
	/* Clear hash slot when key or value is about to be collected. */
	lj_tv_load_acq(&val, &n->val);
	if (!tvisnil(&val)) {
	  lj_tv_load_acq(&key, &n->key);
	  if (gc_mayclear(g, &key, 0) || gc_mayclear(g, &val, 1))
	    lj_tab_storenilraw(&n->val);
	}
      }
    }
    o = gcref_acq(t->gclist);
  }
}

static int gc_call_finalizer(global_State *g, lua_State *L,
			     cTValue *mo, GCobj *o)
{
  /* Save and restore lots of state around the __gc callback. */
  LJStateClaim claim;
  lua_State *cbL = L;
  lua_State *oldL;
  uint8_t oldh;
  GCSize oldt;
  int had_mt_exclusive;
  int continue_gc = 1;
  int errcode;
  ptrdiff_t oldtop;
  TValue *top;
  if (!lj_state_tryclaim(cbL, lj_thr_current_id(g), &claim))
    return 0;  /* Caller must preclaim before clearing FINREG state. */
  lj_assertG(cbL != vmthread_acq(g),
	     "gc_call_finalizer must not use shared vmthread callback stack");
  oldL = lj_tg_cur_L(g);
  oldh = hook_save(g);
  oldt = lj_gc2_finalizer_pause_threshold(g);
  lj_trace_abort(g);
  hook_entergc(g);  /* Disable hooks and new traces during __gc. */
  if (LJ_HASPROFILE && (oldh & HOOK_PROFILE)) lj_dispatch_update(g, 0);
  lj_state_checkstack(cbL, 2+LJ_FR2+LUA_MINSTACK);
  oldtop = savestack(cbL, cbL->top);
  top = cbL->top;
  copyTV(cbL, top++, mo);
  if (LJ_FR2) setnilV(top++);
  setgcV(cbL, top, o, ~o->gch.gct);
  cbL->top = top+1;
  had_mt_exclusive = lj_gc2_finalizer_mt_release_exclusive(g);
  errcode = lj_vm_pcall(cbL, top, 1+0, -1);  /* Stack: |mo|o| -> | */
  if (had_mt_exclusive)
    continue_gc = lj_gc2_finalizer_mt_reclaim_exclusive(g);
  if (oldL)
    lj_tg_setcur_L(g, oldL);
  else
    lj_tg_clearcur_L(g);
  hook_restore(g, oldh);
  if (LJ_HASPROFILE && (oldh & HOOK_PROFILE)) lj_dispatch_update(g, 0);
  lj_gc2_finalizer_restore_threshold(g, oldt);
  if (errcode) {
    TValue tmp;
    copyTV(cbL, &tmp, cbL->top-1);
    cbL->top = restorestack(cbL, oldtop);
    lj_state_dropclaim(&claim);
    lj_vmevent_send(g, ERRFIN,
      copyTV(V, V->top++, &tmp);
    );
  } else {
    cbL->top = restorestack(cbL, oldtop);
    lj_state_dropclaim(&claim);
  }
  return continue_gc;
}

static int gc_dispatch_finalizer_obj(lua_State *L, global_State *g, GCobj *o)
{
#if LJ_HASFFI
  if (o->gch.gct == ~LJ_TCDATA) {
    return lj_gc2_finreg_cdata_dispatch(L, g, o, gc_call_finalizer);
  }
#endif
  {
    return lj_gc2_finreg_udata_dispatch(L, g, o, gc_call_finalizer);
  }
}

/* Finalize all userdata/cdata objects from the GC2 finalizer queue. */
void lj_gc_finalize_udata(lua_State *L)
{
  lj_gc2_finalizer_dispatch_all(L, gc_dispatch_finalizer_obj);
}

#if LJ_HASFFI
static void gc_mark_finreg_tv(global_State *g, cTValue *tv)
{
  gc_marktv(g, tv);
}

int lj_gc_cdata_fin_pending(global_State *g)
{
  return lj_gc2_finreg_cdata_pending(g);
}

/* Finalize all cdata objects from finalizer table. */
void lj_gc_finalize_cdata(lua_State *L)
{
  (void)lj_gc2_finreg_cdata_finalize_close(G(L));
}

void lj_gc_finalize_cdata_disable(global_State *g)
{
  lj_gc2_finreg_cdata_disable(g);
}
#endif

/* Free all remaining GC objects. */
void lj_gc_freeall(global_State *g)
{
  MSize i;
  StrTabHdr *hdr;
  /* Free everything, except super-fixed objects (the main thread). */
  g->gc.currentwhite = LJ_GC_WHITES | LJ_GC_SFIXED;
  gc_fullsweep(g, &g->gc.root);
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

  /* 05 section 5.7.1 legacy atomic fixpoint-round bridge. */
  if (!lj_gc2_mark_complete(g, L, 64, ~(uint32_t)0))
    return 0;

  udsize = lj_gc_separateudata(g, 0);  /* Separate userdata to be finalized. */
  gc_mark_finalizers(g);  /* Mark them. */
  udsize += gc_propagate_gray(g);  /* And propagate the marks. */
  /* 05 section 5.7.1 legacy atomic fixpoint-round bridge. */
  if (!lj_gc2_mark_complete(g, L, 64, ~(uint32_t)0))
    return 0;
  gc2_paranoia_check_fixpoint(g);

  /* All marking done, clear weak tables. */
  lj_gc2_mark_to_weak(g);
#if LJ_HASFFI
  if (lj_gc2_finreg_cdata_finalize_pweak(L, g, gc_mark_finreg_tv))
    (void)gc_propagate_gray(g);
#endif
  {
    GCobj *weak = lj_gc_list_head_acq(&g->gc.weak);
    if (!lj_gc2_weak_complete(g, weak, LJ_GC2_WEAK_DRAIN_BATCH))
      lj_gc_clearweak_legacy(g, weak);
  }
  lj_gc2_weak_to_sweep(g);

  lj_buf_shrink(L, &G2TG(g)->tmpbuf);  /* Shrink temp buffer. */

  /* Prepare for sweep phase. */
  g->gc.currentwhite = (uint8_t)otherwhite(g);  /* Flip current white. */
  g->strempty.marked = g->gc.currentwhite;
  setmref(g->gc.sweep, &g->gc.root);
  g->gc.estimate = lj_gc_total_load(g) - (GCSize)udsize;  /* Initial estimate. */
  return 1;
}

#if LJ_HASJIT
static int gc_jit_defer_fixpoint(global_State *g)
{
  return lj_tg_jit_base(g) != NULL &&
	 g->gc.state == GCSpropagate &&
	 lj_gc_list_head_acq(&g->gc.gray) == NULL &&
	 gc2_phase_acq(g) == LJ_GC2_MARK;
}
#else
#define gc_jit_defer_fixpoint(g)	0
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
    if (gc2_phase_acq(g) == LJ_GC2_MARK) {
      if (gc_jit_defer_fixpoint(g))
	return LJ_MAX_MEM;  /* Root handshakes are run after trace exit. */
      if (lj_gc2_fixpoint_round(g, L, LJ_GC2_WORKER_DRAIN_BATCH) == 0)
	return GCSWEEPCOST;  /* 05 section 5.7.1 bounded propagation fixpoint bridge. */
    }
    g->gc.state = GCSatomic;  /* End of mark phase. */
    return 0;
  case GCSatomic:
    if (lj_tg_jit_base(g))  /* Don't run atomic phase on trace. */
      return LJ_MAX_MEM;
    if (!atomic(g, L))
      return GCSWEEPCOST;
    g->gc.state = GCSsweepstring;  /* Start of sweep phase. */
    g->gc.sweepstr = 0;
    return 0;
  case GCSsweepstring: {
    GCSize old = lj_gc_total_load(g);
    StrTabHdr *hdr = lj_str_tabh_acq(g);
    if (hdr)
      gc_sweepstr(g, &hdr->bucket[g->gc.sweepstr++]);  /* Sweep one chain. */
    if (!hdr || g->gc.sweepstr > hdr->mask)
      g->gc.state = GCSsweep;  /* All string hash chains sweeped. */
    {
      GCSize total = lj_gc_total_load(g);
      lj_assertG(old >= total, "sweep increased memory");
      g->gc.estimate -= old - total;
    }
    return GCSWEEPCOST;
    }
  case GCSsweep: {
    GCSize old = lj_gc_total_load(g);
    setmref(g->gc.sweep, gc_sweep(g, mref(g->gc.sweep, GCRef), GCSWEEPMAX));
    {
      GCSize total = lj_gc_total_load(g);
      lj_assertG(old >= total, "sweep increased memory");
      g->gc.estimate -= old - total;
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
      if (arena_prepare && gc2_cycle_sweep_minor_acq(g))
	(void)lj_gc_sweep_gc2_unmarked(g);
      if (arena_prepare)
	gc_arena_verify_sweep_boundary(g);
      (void)gc_arena_finish_sweep_boundary(g, 0);
      lj_gc2_sweep_legacy_ready(g);
      if (gc_arena_sweep_pending(g))
	return GCSWEEPMAX*GCSWEEPCOST;
      if (lj_gc2_finalizer_queue_pending(g)) {  /* Need finalizations? */
	g->gc.state = GCSfinalize;
      } else {  /* Otherwise skip this phase to help the JIT. */
	if (gc2_legacy_sweep_close(g)) {
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
      int finstep = lj_gc2_finalizer_step(L, gc_dispatch_finalizer_obj,
					  GCFINALIZECOST, &fincost);
      if (finstep != 0)
	return fincost;
    }
    (void)gc_arena_finish_sweep_boundary(g, 1);
    if (gc2_legacy_sweep_close(g)) {
      g->gc.state = GCSpause;  /* End of GC cycle. */
      g->gc.debt = 0;
    }
    return 0;
  default:
    lj_assertG(0, "bad GC state");
    return 0;
  }
}

/* Perform a limited amount of incremental GC steps. */
int LJ_FASTCALL lj_gc_step(lua_State *L)
{
  global_State *g = G(L);
  GCSize lim;
  int32_t ostate = vmstate_load_acq(g);
  setvmstate(g, GC);
  lim = (GCSTEPSIZE/100) * g->gc.stepmul;
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
  } while (sizeof(lim) == 8 ? ((int64_t)lim > 0) : ((int32_t)lim > 0));
  if (g->gc.debt < GCSTEPSIZE) {
    lj_gc_threshold_store(g, lj_gc_total_load(g) + GCSTEPSIZE);
    vmstate_store_rel(g, ostate);
    return -1;
  } else {
    g->gc.debt -= GCSTEPSIZE;
    lj_gc_threshold_store(g, lj_gc_total_load(g));
    vmstate_store_rel(g, ostate);
    return 0;
  }
}

static void gc_step_assist_top(lua_State *L, global_State *g, int legacy_step)
{
  lj_gc2_check_trigger(g, L2TG(L));
  if (!legacy_step)
    legacy_step = lj_gc_total_load(g) >= lj_gc_threshold_load(g);
  if (lj_gc2_hard_limit_reached(g)) {
    gc2_interp_hard_checks_add(g, 1);
    lj_gc2_assist(g, L2TG(L));  /* 05 section 5.11 interpreter assist bridge. */
  }
  if (legacy_step)
    lj_gc_step(L);
}

int LJ_FASTCALL lj_gc_should_step_vm(global_State *g)
{
  return lj_gc_should_step(g);
}

/* Ditto, but fix the stack top first. */
void LJ_FASTCALL lj_gc_step_fixtop(lua_State *L)
{
  global_State *g = G(L);
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
  int legacy_step, hard_step;
  TGState *tg;
  L->base = lj_tg_jit_base(g);
  L->top = curr_topL(L);
  tg = L2TG(L);
  lj_gc2_check_trigger(g, tg);
  legacy_step = lj_gc_total_load(g) >= lj_gc_threshold_load(g);
  hard_step = lj_gc2_hard_limit_reached(g);
  if (hard_step) {
    gc2_jit_hard_checks_add(g, 1);
    lj_gc2_assist(g, tg);  /* 05 section 5.11 trace-side assist bridge. */
  }
  if (legacy_step) {
    while (steps-- > 0 && lj_gc_step(L) == 0)
      ;
  }
  /* Return 1 to force a trace exit. */
  return gc_jit_defer_fixpoint(g) ||
	 (G(L)->gc.state == GCSatomic || G(L)->gc.state == GCSfinalize);
}
#endif

/* Perform a full GC cycle. */
void lj_gc_fullgc(lua_State *L)
{
  global_State *g = G(L);
  int32_t ostate = vmstate_load_acq(g);
  setvmstate(g, GC);
  if (g->gc.state <= GCSatomic) {  /* Caught somewhere in the middle. */
    lj_gc2_legacy_preserve_abort(g);
    setmref(g->gc.sweep, &g->gc.root);  /* Sweep everything (preserving it). */
    lj_gc_list_clear_rel(&g->gc.gray);  /* Reset partial propagation lists. */
    lj_gc_list_clear_rel(&g->gc.grayagain);
    lj_gc_list_clear_rel(&g->gc.weak);
    g->gc.state = GCSsweepstring;  /* Fast forward to the sweep phase. */
    g->gc.sweepstr = 0;
  }
  while (g->gc.state == GCSsweepstring || g->gc.state == GCSsweep)
    gc_onestep(L);  /* Finish sweep. */
  lj_assertG(g->gc.state == GCSfinalize || g->gc.state == GCSpause,
	     "bad GC state");
  /* Now perform a full GC. */
  lj_gc2_force_major(g);
  g->gc.state = GCSpause;
  do {
    gc_onestep(L);
    if (lj_gc2_finalizer_spawn_deferred(g)) {
      vmstate_store_rel(g, ostate);
      return;
    }
  } while (g->gc.state != GCSpause);
  lj_gc2_publish_idle_threshold(g);
  vmstate_store_rel(g, ostate);
}

/* -- Write barriers ------------------------------------------------------ */

/* Barrier for a store to a global root slot. */
void lj_gc_pubroot(lua_State *L, cTValue *tv)
{
  global_State *g = G(L);
  lj_gc2_barrier_tv_g(g, tv);
  if (tviswhite(tv) && (g->gc.state == GCSpropagate ||
			g->gc.state == GCSatomic))
    gc_mark(g, gcV(tv));
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
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic)
    gc_mark(g, v);  /* Move frontier forward. */
  else
    makewhite(g, o);  /* Make it white to avoid the following barrier. */
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
  if (!L || !t || !o)
    return;
  lj_gc2_barrier_obj_pair(L, obj2gco(t), o);
  if (iswhite(o) && isblack(obj2gco(t)))
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
  lj_tv_load_acq(&snap, tv);
  if (!tvisgcv(&snap))
    return;
  lj_gc2_barrier_tv_pair_g(g, obj2gco(uv), &snap);
  if ((TV2MARKED(tv) & LJ_GC_BLACK) && tviswhite(&snap)) {
    if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic)
      gc_mark(g, gcV(&snap));
    else
      TV2MARKED(tv) = (TV2MARKED(tv) & (uint8_t)~LJ_GC_COLORS) | curwhite(g);
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
  lj_gc_linkobj(g, o);  /* CAS-publish closed upvalue on root list. */
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
    head = la_load64_acq(&g->gc.root.gcptr64);  /* M7 root-list snapshot. */
    setgcrefp(next, (void *)(uintptr_t)head);
    lj_obj_setgcwr(o, next);
  } while (!la_cas64(&g->gc.root.gcptr64, &head,
		     (uint64_t)(uintptr_t)&o->gch, LA_REL, LA_ACQ));  /* M7 publish. */
#else
  uint32_t head;
  GCRef next;
  do {
    head = la_load32_acq(&g->gc.root.gcptr32);  /* M7 root-list snapshot. */
    setgcrefp(next, (void *)(uintptr_t)head);
    lj_obj_setgcwr(o, next);
  } while (!la_cas32(&g->gc.root.gcptr32, &head,
		     (uint32_t)(uintptr_t)&o->gch, LA_REL, LA_ACQ));  /* M7 publish. */
#endif
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
  lj_gc_linkobj(g, o);
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

/* Account a dead traversable GC object body for later arena bitmap reclaim. */
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
  return 1;
}
