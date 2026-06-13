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

/* -- Mark phase ---------------------------------------------------------- */

void lj_gc_arena_markobj(global_State *g, GCobj *o)
{
  lj_gc2_markobj(g, o);
}

void lj_gc_arena_markmem(global_State *g, void *p)
{
  (void)lj_gc2_markmem(g, p);
}

static void gc_mark_strtab_mem(global_State *g)
{
  StrTabHdr *hdr;
  hdr = (StrTabHdr *)la_loadptr_acq((void *const *)&g->str.tabh);
  if (hdr)
    lj_gc_arena_markmem(g, hdr);
  for (hdr = (StrTabHdr *)la_loadptr_acq((void *const *)&g->str.retired);
       hdr != NULL;
       hdr = (StrTabHdr *)la_loadptr_acq((void *const *)&hdr->retired_next))
    lj_gc_arena_markmem(g, hdr);
}

static void gc_mark_tab_retired_mem(global_State *g)
{
  TabNodeRetire *ret;
  TabArrayRetire *aret;
  for (ret = (TabNodeRetire *)la_loadptr_acq(
	 (void *const *)&g->tab.retired_nodes);
       ret != NULL;
       ret = (TabNodeRetire *)la_loadptr_acq((void *const *)&ret->next)) {
    lj_gc_arena_markmem(g, ret);
    if (la_load32_acq(&ret->armed))
      lj_gc_arena_markmem(g, lj_tab_node_hdrw(ret->node));
  }
  for (aret = (TabArrayRetire *)la_loadptr_acq(
	 (void *const *)&g->tab.retired_arrays);
       aret != NULL;
       aret = (TabArrayRetire *)la_loadptr_acq((void *const *)&aret->next)) {
    lj_gc_arena_markmem(g, aret);
    if (la_load32_acq(&aret->armed))
      lj_gc_arena_markmem(g, aret->array);
  }
}

static void gc_arena_rebuild_free(global_State *g)
{
  TGState *tg = G2TG(g);
  if (tg && (tg->tg_flags & TGF_ARENA_INTERNAL))
    lj_arena_alloc_rebuild_free(&tg->alloc);
}

static void gc_arena_finish_sweep_boundary(global_State *g)
{
  TGState *tg = G2TG(g);
  if (!tg || !(tg->tg_flags & TGF_ARENA_INTERNAL))
    return;
  if (g->gc2.phase == LJ_GC2_SWEEP && gcref(g->gc.mmudata) == NULL) {
    uint32_t epoch = ++tg->alloc.sweep_epoch;
    lj_arena_alloc_prepare_sweep_kind(&tg->alloc, LJ_ARENAK_TRAVERSABLE);
    lj_arena_alloc_sweep_kind(&tg->alloc, LJ_ARENAK_TRAVERSABLE, epoch, 0);
    lj_arena_alloc_restore_sweep_kind(&tg->alloc, LJ_ARENAK_PLAIN);
  } else {
    gc_arena_rebuild_free(g);
  }
}

#ifdef LUA_USE_ASSERT
static void gc_arena_verify_marked(global_State *g, GCobj *o)
{
  TGState *tg = G2TG(g);
  int marked;
  if (!tg || !(tg->tg_flags & TGF_ARENA_INTERNAL))
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
  if (!tg || !(tg->tg_flags & TGF_ARENA_INTERNAL) ||
      g->gc2.phase != LJ_GC2_SWEEP ||
      gcref(g->gc.mmudata) != NULL)
    return;
  for (o = gcref(g->gc.root); o != NULL; o = gcnext(o)) {
    gc_arena_verify_marked(g, o);
    if (o->gch.gct == ~LJ_TTHREAD) {
      GCobj *uv;
      for (uv = gcref(gco2th(o)->openupval); uv != NULL; uv = gcnext(uv))
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
  if (!gc2_paranoia_liveobj(obj2gco(t)))
    return;
  gc2_paranoia_checkobj(g, obj2gco(t), "table");
  if (t->acap > 0)
    gc2_paranoia_checkmem(g, lj_tab_array_acq(t), "table array");
  {
    Node *node = lj_tab_node_acq(t);
    if (lj_tab_node_hmask_acq(node) > 0)
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
  for (uv = gcref(th->openupval); uv != NULL; uv = gcnext(uv))
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
      gc2_paranoia_checkmem(g, sbx->b, "buffer userdata data");
  }
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
  hdr = (StrTabHdr *)la_loadptr_acq((void *const *)&g->str.tabh);
  if (!hdr)
    return;
  strtab = hdr->bucket;
  for (i = 0; i <= hdr->mask; i++) {
    GCobj *o;
    for (o = lj_str_hashhead(strtab[i]); o != NULL; o = gcnext(o))
      gc2_paranoia_checkobj(g, o, "string");
  }
}

static void gc2_paranoia_check_roots(global_State *g)
{
  GCobj *o;
  for (o = gcref(g->gc.root); o != NULL; o = gcnext(o))
    gc2_paranoia_checkone(g, o);
  o = gcref(g->gc.mmudata);
  if (o) {
    GCobj *root = o;
    do {
      o = gcnext(o);
      gc2_paranoia_checkone(g, o);
    } while (o != root);
  }
}

static void gc2_paranoia_check_rawroots(global_State *g)
{
  StrTabHdr *hdr;
  TabNodeRetire *ret;
  TabArrayRetire *aret;
  hdr = (StrTabHdr *)la_loadptr_acq((void *const *)&g->str.tabh);
  if (hdr)
    gc2_paranoia_checkmem(g, hdr, "string table");
  for (hdr = (StrTabHdr *)la_loadptr_acq((void *const *)&g->str.retired);
       hdr != NULL;
       hdr = (StrTabHdr *)la_loadptr_acq((void *const *)&hdr->retired_next))
    gc2_paranoia_checkmem(g, hdr, "retired string table");
  for (ret = (TabNodeRetire *)la_loadptr_acq(
	 (void *const *)&g->tab.retired_nodes);
       ret != NULL;
       ret = (TabNodeRetire *)la_loadptr_acq((void *const *)&ret->next)) {
    gc2_paranoia_checkmem(g, ret, "retired table node record");
    if (la_load32_acq(&ret->armed))
      gc2_paranoia_checkmem(g, lj_tab_node_hdrw(ret->node),
			    "retired table nodes");
  }
  for (aret = (TabArrayRetire *)la_loadptr_acq(
	 (void *const *)&g->tab.retired_arrays);
       aret != NULL;
       aret = (TabArrayRetire *)la_loadptr_acq((void *const *)&aret->next)) {
    gc2_paranoia_checkmem(g, aret, "retired table array record");
    if (la_load32_acq(&aret->armed))
      gc2_paranoia_checkmem(g, aret->array, "retired table array");
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
      gc2_paranoia_checkmem(g, cts, "ctype state");
      gc2_paranoia_checkmem(g, cts->tab, "ctype table");
      gc2_paranoia_checkmem(g, cts->cb.cbid, "callback ids");
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
    for (tv = (TraceVec *)la_loadptr_acq((void *const *)&J->retiredtracev);
	 tv != NULL;
	 tv = (TraceVec *)la_loadptr_acq((void *const *)&tv->retired_next))
      gc2_paranoia_checkmem(g, tv, "retired trace vector");
    for (mcret = (MCodeRetire *)la_loadptr_acq(
	   (void *const *)&J->retiredmcode);
	 mcret != NULL;
	 mcret = (MCodeRetire *)la_loadptr_acq((void *const *)&mcret->next))
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
  gc2_paranoia_check_roots(g);
  gc2_paranoia_check_strtab(g);
  gc2_paranoia_check_rawroots(g);
}
#else
#define gc2_paranoia_check_fixpoint(g)	((void)0)
#endif

/* Mark a TValue (if needed). */
#define gc_marktv(g, tv) \
  { lj_assertG(!tvisgcv(tv) || (~itype(tv) == gcval(tv)->gch.gct), \
	       "TValue and GC type mismatch"); \
    if (tviswhite(tv)) gc_mark(g, gcV(tv)); }

/* Mark a GCobj (if needed). */
#define gc_markobj(g, o) \
  { if (iswhite(obj2gco(o))) gc_mark(g, obj2gco(o)); }

/* Mark a string object. */
#define gc_mark_str(g, s) \
  (lj_gc_arena_markobj((g), obj2gco(s)), \
   lj_obj_cleargcflags(obj2gco(s), LJ_GC_WHITES))

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
    if (LJ_HASBUFFER && udtype == UDTYPE_BUFFER) {
      SBufExt *sbx = (SBufExt *)uddata(ud);
      GCobj *ref;
      if (!sbufiscoworborrow(sbx))
	lj_gc_arena_markmem(g, sbx->b);
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
      if (th->L)
	gc_markobj(g, obj2gco(th->L));  /* 09 section 9.2 child stack. */
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
    setgcrefr(o->gch.gclist, g->gc.gray);
    setgcref(g->gc.gray, o);
  }
}

/* Mark GC roots. */
static void gc_mark_fixedstr(global_State *g)
{
  MSize i;
  StrTabHdr *hdr;
  GCRef *strtab;
  hdr = (StrTabHdr *)la_loadptr_acq((void *const *)&g->str.tabh);
  if (!hdr)
    return;
  strtab = hdr->bucket;
  for (i = 0; i <= hdr->mask; i++) {
    GCobj *o;
    for (o = lj_str_hashhead(strtab[i]); o != NULL; o = gcnext(o))
      if (lj_obj_gcflags(o) & (LJ_GC_FIXED|LJ_GC_SFIXED))
	lj_gc_arena_markobj(g, o);
  }
}

static void gc_mark_threading_live(global_State *g)
{
  LJThreadLive *node;
  for (node = (LJThreadLive *)la_loadptr_acq((void *const *)&g->threading_live);
       node != NULL;
       node = (LJThreadLive *)la_loadptr_acq((void *const *)&node->next)) {
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
      lj_gc_arena_markmem(g, cts);
      lj_gc_arena_markmem(g, cts->tab);
      lj_gc_arena_markmem(g, cts->cb.cbid);
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
  lj_gc2_legacy_mark_begin(g);
  setgcrefnull(g->gc.gray);
  setgcrefnull(g->gc.grayagain);
  setgcrefnull(g->gc.weak);
  gc_markobj(g, mainthread(g));
  {
    GCtab *env = tabref_acq(mainthread(g)->env);
    if (env)
      gc_markobj(g, env);
  }
  gc_markobj(g, vmthread(g));
  gc_marktv(g, &g->registrytv);
  gc_mark_gcroot(g);
  g->gc.state = GCSpropagate;
}

/* Mark open upvalues. */
static void gc_mark_uv(global_State *g)
{
  GCupval *uv;
  for (uv = uvnext(&g->uvhead); uv != &g->uvhead; uv = uvnext(uv)) {
    lj_assertG(uvprev(uvnext(uv)) == uv && uvnext(uvprev(uv)) == uv,
	       "broken upvalue chain");
    if (isgray(obj2gco(uv))) {
      TValue tv;
      lj_tv_load_acq(&tv, uvval(uv));
      gc_marktv(g, &tv);
    }
  }
}

/* Mark userdata in mmudata list. */
static void gc_mark_mmudata(global_State *g)
{
  GCobj *root = gcref(g->gc.mmudata);
  GCobj *u = root;
  if (u) {
    do {
      u = gcnext(u);
      makewhite(g, u);  /* Could be from previous GC. */
      gc_mark(g, u);
    } while (u != root);
  }
}

/* Separate userdata objects to be finalized to mmudata list. */
size_t lj_gc_separateudata(global_State *g, int all)
{
  size_t m = 0;
  GCRef *p = lj_obj_gcwref(obj2gco(mainthread(g)));
  GCobj *o;
  TValue mmv;
  while ((o = gcref(*p)) != NULL) {
    if (!(iswhite(o) || all) || isfinalized(gco2ud(o))) {
      p = lj_obj_gcwref(o);  /* Nothing to do. */
    } else if (!lj_meta_fasttv(g, tabref_acq(gco2ud(o)->metatable),
			       MM_gc, &mmv)) {
      markfinalized(o);  /* Done, as there's no __gc metamethod. */
      p = lj_obj_gcwref(o);
    } else {  /* Otherwise move userdata to be finalized to mmudata list. */
      m += sizeudata(gco2ud(o));
      markfinalized(o);
      *p = *lj_obj_gcwref(o);
      if (gcref(g->gc.mmudata)) {  /* Link to end of mmudata list. */
	GCobj *root = gcref(g->gc.mmudata);
	lj_obj_setgcwr(o, *lj_obj_gcwref(root));
	setgcref(*lj_obj_gcwref(root), o);
	setgcref(g->gc.mmudata, o);
      } else {  /* Create circular list. */
	lj_obj_setgcw(o, o);
	setgcref(g->gc.mmudata, o);
      }
    }
  }
  return m;
}

/* -- Propagation phase --------------------------------------------------- */

/* Traverse a table. */
static int gc_traverse_tab(global_State *g, GCtab *t)
{
  int weak = 0;
  TValue modev;
  cTValue *mode;
  GCtab *mt = tabref_acq(t->metatable);
  if (t->acap > 0)
    lj_gc_arena_markmem(g, lj_tab_array_acq(t));
  {
    Node *node = lj_tab_node_acq(t);
    if (lj_tab_node_hmask_acq(node) > 0)
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
      if (gcref_acq(g->gcroot[GCROOT_FFI_FIN]) == obj2gco(t)) {
	weak = (int)(~0u & ~LJ_GC_WEAKVAL);
      } else
#endif
      {
	lj_obj_masksetgcflags(obj2gco(t), LJ_GC_WEAK, weak);
	setgcrefr(t->gclist, g->gc.weak);
	setgcref(g->gc.weak, obj2gco(t));
      }
    }
  }
  if (weak == LJ_GC_WEAK)  /* Nothing to mark if both keys/values are weak. */
    return 1;
  if (!(weak & LJ_GC_WEAKVAL)) {  /* Mark array part. */
    MSize i, asize = lj_tab_asize_acq(t);
    TValue *array = lj_tab_array_acq(t);
    for (i = 0; i < asize; i++) {
      TValue val;
      lj_tv_load_acq(&val, &array[i]);
      gc_marktv(g, &val);
    }
  }
  {  /* Mark hash part. */
    Node *node = lj_tab_node_acq(t);
    MSize i, hmask = lj_tab_node_hmask_acq(node);
    if (hmask > 0) {
      for (i = 0; i <= hmask; i++) {
	Node *n = &node[i];
	TValue key, val;
	lj_tv_load_acq(&val, &n->val);
	if (!tvisnil(&val)) {  /* Mark non-empty slot. */
	  lj_tv_load_acq(&key, &n->key);
	  lj_assertG(!tvisnil(&key), "mark of nil key in non-empty slot");
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
      gc_markobj(g, &gcref(fn->l.uvptr[i])->uv);
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
    setgcrefr(o->gch.gclist, g->gc.gray);
    setgcref(g->gc.gray, o);
  }
}

/* Traverse a trace. */
static void gc_traverse_trace(global_State *g, GCtrace *T)
{
  IRRef ref;
  if (T->traceno == 0) return;
  for (ref = T->nk; ref < REF_TRUE; ref++) {
    IRIns *ir = &T->ir[ref];
    if (ir->o == IR_KGC)
      gc_markobj(g, ir_kgc(ir));
    if (irt_is64(ir->t) && ir->o != IR_KNULL)
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
  gc_markobj(g, gcref(T->startpt));
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
  gc_mark_str(g, proto_chunkname(pt));
  for (i = -(ptrdiff_t)pt->sizekgc; i < 0; i++)  /* Mark collectable consts. */
    gc_markobj(g, proto_kgc(pt, i));
#if LJ_HASJIT
  {
    TraceNo trace = proto_trace_acq(pt);
    if (trace) gc_marktrace(g, trace);
  }
#endif
}

/* Traverse the frame structure of a stack. */
static MSize gc_traverse_frames(global_State *g, lua_State *th)
{
  TValue *frame, *top = th->top-1, *bot = tvref(th->stack);
  /* Note: extra vararg frame not skipped, marks function twice (harmless). */
  for (frame = th->base-1; frame > bot+LJ_FR2; frame = frame_prev(frame)) {
    GCfunc *fn = frame_func(frame);
    TValue *ftop = frame;
    if (isluafunc(fn)) ftop += funcproto(fn)->framesize;
    if (ftop > top) top = ftop;
    if (!LJ_FR2) gc_markobj(g, fn);  /* Need to mark hidden function (or L). */
  }
  top++;  /* Correct bias of -1 (frame == base-1). */
  if (top > tvref(th->maxstack)) top = tvref(th->maxstack);
  return (MSize)(top - bot);  /* Return minimum needed stack size. */
}

/* Traverse a thread object. */
static void gc_traverse_thread(global_State *g, lua_State *th)
{
  GCobj *mt;
  TValue *o, *top = th->top;
  TValue tv;
  lj_gc_arena_markmem(g, tvref(th->stack));
  for (o = tvref(th->stack)+1+LJ_FR2; o < top; o++) {
    lj_tv_load_acq(&tv, o);
    gc_marktv(g, &tv);
  }
  if (g->gc.state == GCSatomic) {
    top = tvref(th->stack) + th->stacksize;
    for (; o < top; o++)  /* Clear unmarked slots. */
      setnilV(o);
  }
  {
    GCtab *env = tabref_acq(th->env);
    if (env)
      gc_markobj(g, env);
  }
  mt = gcref_acq(th->mt_thread);
  if (mt != NULL)
    gc_markobj(g, mt);
  lj_state_shrinkstack(th, gc_traverse_frames(g, th));
}

/* Propagate one gray object. Traverse it and turn it black. */
static size_t propagatemark(global_State *g)
{
  GCobj *o = gcref(g->gc.gray);
  int gct = o->gch.gct;
  lj_assertG(isgray(o), "propagation of non-gray object");
  gray2black(o);
  setgcrefr(g->gc.gray, o->gch.gclist);  /* Remove from gray list. */
  if (LJ_LIKELY(gct == ~LJ_TTAB)) {
    GCtab *t = gco2tab(o);
    Node *node = lj_tab_node_acq(t);
    MSize hmask = lj_tab_node_hmask_acq(node);
    if (gc_traverse_tab(g, t) > 0)
      black2gray(o);  /* Keep weak tables gray. */
    return (LJ_MAX_COLOSIZE != 0 && t->colo ?
	    sizetabcolo((uint32_t)t->colo & 0x7f) : sizeof(GCtab)) +
	   (t->acap && (LJ_MAX_COLOSIZE == 0 || t->colo <= 0) ?
	    sizeof(TValue) * t->acap : 0) +
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
    setgcrefr(th->gclist, g->gc.grayagain);
    setgcref(g->gc.grayagain, o);
    black2gray(o);  /* Threads are never black. */
    gc_traverse_thread(g, th);
    return sizeof(lua_State) + sizeof(TValue) * th->stacksize;
  } else {
#if LJ_HASJIT
    GCtrace *T = gco2trace(o);
    gc_traverse_trace(g, T);
    return ((sizeof(GCtrace)+7)&~7) + (T->nins-T->nk)*sizeof(IRIns) +
	   T->nsnap*sizeof(SnapShot) + T->nsnapmap*sizeof(SnapEntry);
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
  while (gcref(g->gc.gray) != NULL)
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

/* Full sweep of a GC list. */
#define gc_fullsweep(g, p)	gc_sweep(g, (p), ~(uint32_t)0)

/* Partial sweep of a GC list. */
static GCRef *gc_sweep(global_State *g, GCRef *p, uint32_t lim)
{
  /* Mask with other white and LJ_GC_FIXED. Or LJ_GC_SFIXED on shutdown. */
  int ow = otherwhite(g);
  GCobj *o;
  while ((o = gcref(*p)) != NULL && lim-- > 0) {
    if (o->gch.gct == ~LJ_TTHREAD)  /* Need to sweep open upvalues, too. */
      gc_fullsweep(g, &gco2th(o)->openupval);
    if (((lj_obj_gcflags(o) ^ LJ_GC_WHITES) & ow)) {  /* Black or current white? */
      lj_assertG(!isdead(g, o) || (lj_obj_gcflags(o) & LJ_GC_FIXED),
		 "sweep of undead object");
      makewhite(g, o);  /* Value is alive, change to the current white. */
      p = lj_obj_gcwref(o);
    } else {  /* Otherwise value is dead, free it. */
      lj_assertG(isdead(g, o) || ow == LJ_GC_SFIXED,
		 "sweep of unlive object");
      setgcrefr(*p, *lj_obj_gcwref(o));
      if (o == gcref(g->gc.root))
	setgcrefr(g->gc.root, *lj_obj_gcwref(o));  /* Adjust list anchor. */
      gc_freefunc[o->gch.gct - ~LJ_TSTR](g, o);
    }
  }
  return p;
}

/* Sweep one string interning table chain. Preserves hashalg bit. */
static void gc_sweepstr(global_State *g, GCRef *chain)
{
  /* Mask with other white and LJ_GC_FIXED. Or LJ_GC_SFIXED on shutdown. */
  int ow = otherwhite(g);
  uintptr_t u = gcrefu(*chain);
  GCRef q;
  GCRef *p = &q;
  GCobj *o;
  setgcrefp(q, (u & ~(uintptr_t)LJ_STRHASH_LINKMASK));
  while ((o = gcref(*p)) != NULL) {
    if (((lj_obj_gcflags(o) ^ LJ_GC_WHITES) & ow)) {  /* Black or current white? */
      lj_assertG(!isdead(g, o) || (lj_obj_gcflags(o) & LJ_GC_FIXED),
		 "sweep of undead string");
      makewhite(g, o);  /* String is alive, change to the current white. */
      p = lj_obj_gcwref(o);
    } else {  /* Otherwise string is dead, free it. */
      lj_assertG(isdead(g, o) || ow == LJ_GC_SFIXED,
		 "sweep of unlive string");
      setgcrefr(*p, *lj_obj_gcwref(o));
      lj_str_free(g, gco2str(o));
    }
  }
  setgcrefp(*chain, (gcrefu(q) | (u & LJ_STRHASH_SECONDARY)));
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
static void gc_clearweak(global_State *g, GCobj *o)
{
  while (o) {
    GCtab *t = gco2tab(o);
    lj_assertG((lj_obj_gcflags(obj2gco(t)) & LJ_GC_WEAK),
	       "clear of non-weak table");
    if ((lj_obj_gcflags(obj2gco(t)) & LJ_GC_WEAKVAL)) {
      MSize i, asize = lj_tab_asize_acq(t);
      TValue *array = lj_tab_array_acq(t);
      for (i = 0; i < asize; i++) {
	/* Clear array slot when value is about to be collected. */
	TValue val;
	lj_tv_load_acq(&val, &array[i]);
	if (gc_mayclear(g, &val, 1))
	  lj_tab_storenilraw(&array[i]);
      }
    }
    {
      Node *node = lj_tab_node_acq(t);
      MSize i, hmask = lj_tab_node_hmask_acq(node);
      if (hmask > 0) {
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
    }
    o = gcref(t->gclist);
  }
}

/* Call a userdata or cdata finalizer. */
static void gc_call_finalizer(global_State *g, lua_State *L,
			      cTValue *mo, GCobj *o)
{
  /* Save and restore lots of state around the __gc callback. */
  uint8_t oldh = hook_save(g);
  GCSize oldt = lj_gc_threshold_load(g);
  int errcode;
  lua_State *VL = vmthread(g);
  TValue *top;
  lj_trace_abort(g);
  hook_entergc(g);  /* Disable hooks and new traces during __gc. */
  if (LJ_HASPROFILE && (oldh & HOOK_PROFILE)) lj_dispatch_update(g, 0);
  lj_gc_threshold_store(g, LJ_MAX_MEM);  /* Prevent GC steps. */
  top = VL->top;
  copyTV(VL, top++, mo);
  if (LJ_FR2) setnilV(top++);
  setgcV(VL, top, o, ~o->gch.gct);
  VL->top = top+1;
  errcode = lj_vm_pcall(VL, top, 1+0, -1);  /* Stack: |mo|o| -> | */
  lj_tg_setcur_L(g, L);
  hook_restore(g, oldh);
  if (LJ_HASPROFILE && (oldh & HOOK_PROFILE)) lj_dispatch_update(g, 0);
  lj_gc_threshold_store(g, oldt);  /* Restore GC threshold. */
  if (errcode) {
    TValue tmp;
    copyTV(VL, &tmp, VL->top-1);
    VL->top--;
    lj_vmevent_send(g, ERRFIN,
      copyTV(V, V->top++, &tmp);
    );
  }
}

/* Finalize one userdata or cdata object from the mmudata list. */
static void gc_finalize(lua_State *L)
{
  global_State *g = G(L);
  GCobj *o = gcnext(gcref(g->gc.mmudata));
  cTValue *mo;
  TValue motv;
  lj_assertG(lj_tg_jit_base(g) == NULL, "finalizer called on trace");
  /* Unchain from list of userdata to be finalized. */
  if (o == gcref(g->gc.mmudata))
    setgcrefnull(g->gc.mmudata);
  else
    setgcrefr(*lj_obj_gcwref(gcref(g->gc.mmudata)), *lj_obj_gcwref(o));
#if LJ_HASFFI
  if (o->gch.gct == ~LJ_TCDATA) {
    TValue tmp, *tv;
    /* Add cdata back to the GC list and make it white. */
    lj_obj_setgcwr(o, g->gc.root);
    setgcref(g->gc.root, o);
    makewhite(g, o);
    lj_gc_arena_markobj(g, o);
    lj_obj_cleargcflags(o, LJ_GC_CDATA_FIN);
    /* Resolve finalizer. */
    setcdataV(L, &tmp, gco2cd(o));
    tv = lj_tab_set(L, gco2tab(gcref_acq(g->gcroot[GCROOT_FFI_FIN])), &tmp);
    if (!tvisnil(tv)) {
      copyTV(L, &tmp, tv);
      lj_tab_storenilraw(tv);  /* Clear entry in finalizer table. */
      gc_call_finalizer(g, L, &tmp, o);
    }
    return;
  }
#endif
  /* Add userdata back to the main userdata list and make it white. */
  lj_obj_setgcwr(o, *lj_obj_gcwref(obj2gco(mainthread(g))));
  setgcref(*lj_obj_gcwref(obj2gco(mainthread(g))), o);
  makewhite(g, o);
  lj_gc_arena_markobj(g, o);
  /* Resolve the __gc metamethod. */
  mo = lj_meta_fasttv(g, tabref_acq(gco2ud(o)->metatable), MM_gc, &motv);
  if (mo)
    gc_call_finalizer(g, L, mo, o);
}

/* Finalize all userdata objects from mmudata list. */
void lj_gc_finalize_udata(lua_State *L)
{
  while (gcref(G(L)->gc.mmudata) != NULL)
    gc_finalize(L);
}

#if LJ_HASFFI
/* Finalize all cdata objects from finalizer table. */
void lj_gc_finalize_cdata(lua_State *L)
{
  global_State *g = G(L);
  GCtab *t = gco2tab(gcref_acq(g->gcroot[GCROOT_FFI_FIN]));
  Node *node = lj_tab_node_acq(t);
  MSize hmask = lj_tab_node_hmask_acq(node);
  ptrdiff_t i;
  setgcrefnull(t->metatable);  /* Mark finalizer table as disabled. */
  for (i = (ptrdiff_t)hmask; i >= 0; i--) {
    TValue key, val;
    lj_tv_load_acq(&val, &node[i].val);
    if (!tvisnil(&val)) {
      lj_tv_load_acq(&key, &node[i].key);
      if (tviscdata(&key)) {
	GCobj *o = gcV(&key);
	TValue tmp;
	makewhite(g, o);
	lj_obj_cleargcflags(o, LJ_GC_CDATA_FIN);
	copyTV(L, &tmp, &val);
	lj_tab_storenilraw(&node[i].val);
	gc_call_finalizer(g, L, &tmp, o);
      }
    }
  }
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
  hdr = (StrTabHdr *)la_loadptr_acq((void *const *)&g->str.tabh);
  if (hdr)
    for (i = hdr->mask; i != ~(MSize)0; i--)  /* Free all string hash chains. */
      gc_sweepstr(g, &hdr->bucket[i]);
}

/* -- Collector ----------------------------------------------------------- */

/* Atomic part of the GC cycle, transitioning from mark to sweep phase. */
static void atomic(global_State *g, lua_State *L)
{
  size_t udsize;

  gc_mark_uv(g);  /* Need to remark open upvalues (the thread may be dead). */
  gc_propagate_gray(g);  /* Propagate any left-overs. */

  setgcrefr(g->gc.gray, g->gc.weak);  /* Empty the list of weak tables. */
  setgcrefnull(g->gc.weak);
  lj_assertG(!iswhite(obj2gco(mainthread(g))), "main thread turned white");
  gc_markobj(g, L);  /* Mark running thread. */
  gc_traverse_curtrace(g);  /* Traverse current trace. */
  gc_mark_gcroot(g);  /* Mark GC roots (again). */
  gc_propagate_gray(g);  /* Propagate all of the above. */

  setgcrefr(g->gc.gray, g->gc.grayagain);  /* Empty the 2nd chance list. */
  setgcrefnull(g->gc.grayagain);
  gc_propagate_gray(g);  /* Propagate it. */

  udsize = lj_gc_separateudata(g, 0);  /* Separate userdata to be finalized. */
  gc_mark_mmudata(g);  /* Mark them. */
  udsize += gc_propagate_gray(g);  /* And propagate the marks. */
  /* 05 section 5.7.1 legacy atomic fixpoint-round bridge. */
  (void)lj_gc2_fixpoint_run(g, L, 64, ~(uint32_t)0);
  gc2_paranoia_check_fixpoint(g);

  /* All marking done, clear weak tables. */
  gc_clearweak(g, gcref(g->gc.weak));
  lj_gc2_legacy_sweep_begin(g);

  lj_buf_shrink(L, &G2TG(g)->tmpbuf);  /* Shrink temp buffer. */

  /* Prepare for sweep phase. */
  g->gc.currentwhite = (uint8_t)otherwhite(g);  /* Flip current white. */
  g->strempty.marked = g->gc.currentwhite;
  setmref(g->gc.sweep, &g->gc.root);
  g->gc.estimate = g->gc.total - (GCSize)udsize;  /* Initial estimate. */
}

/* GC state machine. Returns a cost estimate for each step performed. */
static size_t gc_onestep(lua_State *L)
{
  global_State *g = G(L);
  switch (g->gc.state) {
  case GCSpause:
    gc_mark_start(g);  /* Start a new GC cycle by marking all GC roots. */
    return 0;
  case GCSpropagate:
    if (gcref(g->gc.gray) != NULL)
      return propagatemark(g);  /* Propagate one gray object. */
    g->gc.state = GCSatomic;  /* End of mark phase. */
    return 0;
  case GCSatomic:
    if (lj_tg_jit_base(g))  /* Don't run atomic phase on trace. */
      return LJ_MAX_MEM;
    atomic(g, L);
    g->gc.state = GCSsweepstring;  /* Start of sweep phase. */
    g->gc.sweepstr = 0;
    return 0;
  case GCSsweepstring: {
    GCSize old = g->gc.total;
    StrTabHdr *hdr = (StrTabHdr *)la_loadptr_acq((void *const *)&g->str.tabh);
    if (hdr)
      gc_sweepstr(g, &hdr->bucket[g->gc.sweepstr++]);  /* Sweep one chain. */
    if (!hdr || g->gc.sweepstr > hdr->mask)
      g->gc.state = GCSsweep;  /* All string hash chains sweeped. */
    lj_assertG(old >= g->gc.total, "sweep increased memory");
    g->gc.estimate -= old - g->gc.total;
    return GCSWEEPCOST;
    }
  case GCSsweep: {
    GCSize old = g->gc.total;
    setmref(g->gc.sweep, gc_sweep(g, mref(g->gc.sweep, GCRef), GCSWEEPMAX));
    lj_assertG(old >= g->gc.total, "sweep increased memory");
    g->gc.estimate -= old - g->gc.total;
    if (gcref(*mref(g->gc.sweep, GCRef)) == NULL) {
      StrTabHdr *hdr = (StrTabHdr *)la_loadptr_acq((void *const *)&g->str.tabh);
      MSize mask = hdr ? hdr->mask : ~(MSize)0;
      if (la_load32_acq(&g->str.num) <= (mask >> 2) &&
	  mask > LJ_MIN_STRTAB*2-1)
	lj_str_resize(L, mask >> 1);  /* Shrink string table. */
      gc_arena_verify_sweep_boundary(g);
      gc_arena_finish_sweep_boundary(g);
      if (gcref(g->gc.mmudata)) {  /* Need any finalizations? */
	g->gc.state = GCSfinalize;
      } else {  /* Otherwise skip this phase to help the JIT. */
	g->gc.state = GCSpause;  /* End of GC cycle. */
	lj_gc2_legacy_cycle_end(g);
	g->gc.debt = 0;
      }
    }
    return GCSWEEPMAX*GCSWEEPCOST;
    }
  case GCSfinalize:
    if (gcref(g->gc.mmudata) != NULL) {
      GCSize old = g->gc.total;
      if (lj_tg_jit_base(g))  /* Don't call finalizers on trace. */
	return LJ_MAX_MEM;
      gc_finalize(L);  /* Finalize one userdata object. */
      if (old >= g->gc.total && g->gc.estimate > old - g->gc.total)
	g->gc.estimate -= old - g->gc.total;
      if (g->gc.estimate > GCFINALIZECOST)
	g->gc.estimate -= GCFINALIZECOST;
      return GCFINALIZECOST;
    }
    gc_arena_finish_sweep_boundary(g);
    g->gc.state = GCSpause;  /* End of GC cycle. */
    lj_gc2_legacy_cycle_end(g);
    g->gc.debt = 0;
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
  int32_t ostate = g->vmstate;
  setvmstate(g, GC);
  lim = (GCSTEPSIZE/100) * g->gc.stepmul;
  if (lim == 0)
    lim = LJ_MAX_MEM;
  {
    GCSize threshold = lj_gc_threshold_load(g);
    if (g->gc.total > threshold)
      g->gc.debt += g->gc.total - threshold;
  }
  do {
    lim -= (GCSize)gc_onestep(L);
    if (g->gc.state == GCSpause) {
      lj_gc_threshold_store(g, (g->gc.estimate/100) * g->gc.pause);
      g->vmstate = ostate;
      return 1;  /* Finished a GC cycle. */
    }
  } while (sizeof(lim) == 8 ? ((int64_t)lim > 0) : ((int32_t)lim > 0));
  if (g->gc.debt < GCSTEPSIZE) {
    lj_gc_threshold_store(g, g->gc.total + GCSTEPSIZE);
    g->vmstate = ostate;
    return -1;
  } else {
    g->gc.debt -= GCSTEPSIZE;
    lj_gc_threshold_store(g, g->gc.total);
    g->vmstate = ostate;
    return 0;
  }
}

/* Ditto, but fix the stack top first. */
void LJ_FASTCALL lj_gc_step_fixtop(lua_State *L)
{
  if (curr_funcisL(L)) L->top = curr_topL(L);
  lj_gc_step(L);
}

#if LJ_HASJIT
/* Perform multiple GC steps. Called from JIT-compiled code. */
int LJ_FASTCALL lj_gc_step_jit(global_State *g, MSize steps)
{
  lua_State *L = lj_tg_cur_L(g);
  int legacy_step, hard_step;
  L->base = lj_tg_jit_base(g);
  L->top = curr_topL(L);
  legacy_step = g->gc.total >= lj_gc_threshold_load(g);
  hard_step = la_load64_acq(&g->gc2.alloc_since_trigger) >
	      la_load64_acq(&g->gc2.hard_bytes);
  if (hard_step)
    la_add64_rlx(&g->gc2.jit_hard_checks, 1);
  lj_gc2_assist(g, L2TG(L));  /* 05 section 5.11 trace-side assist bridge. */
  if (legacy_step) {
    while (steps-- > 0 && lj_gc_step(L) == 0)
      ;
  }
  /* Return 1 to force a trace exit. */
  return (G(L)->gc.state == GCSatomic || G(L)->gc.state == GCSfinalize);
}
#endif

/* Perform a full GC cycle. */
void lj_gc_fullgc(lua_State *L)
{
  global_State *g = G(L);
  int32_t ostate = g->vmstate;
  setvmstate(g, GC);
  if (g->gc.state <= GCSatomic) {  /* Caught somewhere in the middle. */
    lj_gc2_legacy_preserve_abort(g);
    setmref(g->gc.sweep, &g->gc.root);  /* Sweep everything (preserving it). */
    setgcrefnull(g->gc.gray);  /* Reset lists from partial propagation. */
    setgcrefnull(g->gc.grayagain);
    setgcrefnull(g->gc.weak);
    g->gc.state = GCSsweepstring;  /* Fast forward to the sweep phase. */
    g->gc.sweepstr = 0;
  }
  while (g->gc.state == GCSsweepstring || g->gc.state == GCSsweep)
    gc_onestep(L);  /* Finish sweep. */
  lj_assertG(g->gc.state == GCSfinalize || g->gc.state == GCSpause,
	     "bad GC state");
  /* Now perform a full GC. */
  g->gc.state = GCSpause;
  do { gc_onestep(L); } while (g->gc.state != GCSpause);
  lj_gc_threshold_store(g, (g->gc.estimate/100) * g->gc.pause);
  g->vmstate = ostate;
}

/* -- Write barriers ------------------------------------------------------ */

/* Barrier for a store to a global root slot. */
void lj_gc_barrierroot(lua_State *L, cTValue *tv)
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

/* Publication wrapper for closed-upvalue TValue stores. Pass &uv->tv. */
void LJ_FASTCALL lj_gc_pubuv(global_State *g, TValue *tv)
{
#define TV2MARKED(x) \
  (*((uint8_t *)(x) - offsetof(GCupval, tv) + offsetof(GCupval, marked)))
  TValue snap;
  lj_tv_load_acq(&snap, tv);
  lj_gc2_barrier_uv(g, &snap);
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
  copyTVrel(mainthread(g), &uv->tv, uvval(uv));
  setmref(uv->v, &uv->tv);
  uv->closed = 1;
  lj_obj_setgcwr(o, g->gc.root);
  setgcref(g->gc.root, o);
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
void lj_gc_barriertrace(global_State *g, uint32_t traceno)
{
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic)
    gc_marktrace(g, traceno);
}
#endif

/* -- Allocator ----------------------------------------------------------- */

static LJArenaAllocD *gc_arena_allocd_for_tg(global_State *g, TGState *tg)
{
  if (tg && (tg->tg_flags & TGF_ARENA_INTERNAL))
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
    uint32_t owner_tid = lj_arena_of(p)->hdr.owner_tid;
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
  g->gc.total = (g->gc.total - osz) + nsz;
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
  g->gc.total += size;
  lj_gc2_account_alloc(g, L2TG(L), size);  /* 04 section 4.8. */
  return o;
}

/* Allocate new GC object and link it to the root set. */
void * LJ_FASTCALL lj_mem_newgco(lua_State *L, GCSize size)
{
  global_State *g = G(L);
  GCobj *o = (GCobj *)lj_mem_newgco_raw(L, size, LJ_AF_TRAVERSABLE);
  lj_obj_setgcwr(o, g->gc.root);
  setgcref(g->gc.root, o);
  newwhite(g, o);
  return o;
}

void lj_mem_free(global_State *g, void *p, size_t osize)
{
  g->gc.total -= (GCSize)osize;
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
  if (!p || !tg || !(tg->tg_flags & TGF_ARENA_INTERNAL) ||
      g->allocf != lj_arena_allocf)
    return 0;
  a = lj_arena_of(p);
  if (lj_arena_ishuge(a) || !(a->hdr.flags & LJ_AF_TRAVERSABLE))
    return 0;
  cell = lj_arena_cellof(p);
  if (cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS ||
      !lj_arena_bm_get(a->block, cell))
    return 0;
  g->gc.total -= osize;
  return 1;
}
