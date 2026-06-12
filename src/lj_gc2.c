/*
** Concurrent GC scaffold.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_gc2_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc2.h"
#include "lj_gc.h"
#include "lj_safepoint.h"
#include "lj_arena.h"
#include "lj_tg.h"
#include "lj_frame.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#endif
#include "lj_dispatch.h"

static void gc2_attach_main(global_State *g)
{
  TGState *tg = G2TG(g);
  g->gc2.tg_list = tg;
  g->gc2.n_threads = tg ? 1u : 0u;
  if (tg) {
    tg->poll = 0;
    tg->reqmask = 0;
    tg->hs_epoch_ack = g->gc2.hs_epoch;
    tg->next_tg = NULL;
  }
}

void lj_gc2_init(global_State *g)
{
  g->gc2.phase = LJ_GC2_IDLE;
  g->gc2.cycle = 0;
  g->gc2.hs_epoch = 0;
  g->gc2.hs_pending = 0;
  g->gc2.hs_actions = 0;
  g->gc2.marks_this_round = 0;
  g->gc2.ssb_head = NULL;
  g->gc2.ssb_published = 0;
  g->gc2.ssb_drained = 0;
  g->gc2.ssb_items_published = 0;
  g->gc2.ssb_items_drained = 0;
  gc2_attach_main(g);
}

static void gc2_clear_marks(global_State *g, TGState *tg)
{
  if (tg && (tg->tg_flags & TGF_ARENA_INTERNAL)) {
    lj_arena_alloc_clear_marks(&tg->alloc);
    if (tg->tg_flags & TGF_HUGETAB)
      lj_arena_hugetab_clear_marks(&tg->huge);
  }
}

void lj_gc2_legacy_mark_begin(global_State *g)
{
  TGState *tg = G2TG(g);
  if (g->gc2.tg_list == NULL && tg != NULL)
    gc2_attach_main(g);
  g->gc2.phase = LJ_GC2_MARK;
  g->gc2.cycle++;
  g->gc2.marks_this_round = 0;
  gc2_clear_marks(g, tg);
  lj_gc2_handshake(g, LJ_GC2_HS_ENABLE_BARRIER|LJ_GC2_HS_ALLOC_BLACK);
}

void lj_gc2_legacy_sweep_begin(global_State *g)
{
  g->gc2.phase = LJ_GC2_SWEEP;
  lj_gc2_handshake(g, LJ_GC2_HS_DISABLE_BARRIER|LJ_GC2_HS_RESET_ALLOC|
		   LJ_GC2_HS_FLUSH_SSB);
  (void)lj_gc2_drain_ssb(g);  /* Temporary worker-consume stand-in. */
}

void lj_gc2_legacy_preserve_abort(global_State *g)
{
  g->gc2.phase = LJ_GC2_IDLE;
  lj_gc2_handshake(g, LJ_GC2_HS_DISABLE_BARRIER|LJ_GC2_HS_ALLOC_WHITE|
		   LJ_GC2_HS_FLUSH_SSB);
  (void)lj_gc2_drain_ssb(g);
}

void lj_gc2_legacy_cycle_end(global_State *g)
{
  g->gc2.phase = LJ_GC2_IDLE;
  lj_gc2_handshake(g, LJ_GC2_HS_DISABLE_BARRIER|LJ_GC2_HS_ALLOC_WHITE|
		   LJ_GC2_HS_FLUSH_SSB);
  (void)lj_gc2_drain_ssb(g);
}

uint32_t lj_gc2_handshake(global_State *g, uint32_t actions)
{
  return lj_safepoint_handshake(g, actions);
}

static void gc2_mark_tv(global_State *g, cTValue *tv)
{
  if (tvisgcv(tv))
    lj_gc2_markobj(g, gcV(tv));
}

static void gc2_mark_fixedstr(global_State *g)
{
  MSize i;
  if (!g->str.tab || g->str.mask == ~(MSize)0)
    return;
  for (i = 0; i <= g->str.mask; i++) {
    GCobj *o;
    for (o = gcref(g->str.tab[i]); o != NULL; o = gcnext(o))
      if (lj_obj_gcflags(o) & (LJ_GC_FIXED|LJ_GC_SFIXED))
	lj_gc2_markobj(g, o);
  }
}

static TValue *gc2_stack_scan_top(global_State *g, lua_State *L)
{
  TValue *frame, *top = L->top - 1, *bot = tvref(L->stack);
  for (frame = L->base - 1; frame > bot + LJ_FR2; frame = frame_prev(frame)) {
    GCfunc *fn = frame_func(frame);
    TValue *ftop = frame;
    if (isluafunc(fn))
      ftop += funcproto(fn)->framesize;
    if (ftop > top)
      top = ftop;
    if (!LJ_FR2)
      lj_gc2_markobj(g, obj2gco(fn));
  }
  top++;
  if (top > tvref(L->maxstack))
    top = tvref(L->maxstack);
  return top;
}

static void gc2_scan_thread_roots(global_State *g, lua_State *L)
{
  GCobj *uv;
  TValue *o, *top;
  if (!L || tvref(L->stack) == NULL)
    return;
  lj_gc2_markobj(g, obj2gco(L));
  lj_gc2_markmem(g, tvref(L->stack));
  top = gc2_stack_scan_top(g, L);
  for (o = tvref(L->stack) + 1 + LJ_FR2; o < top; o++)
    gc2_mark_tv(g, o);
  if (tabref(L->env))
    lj_gc2_markobj(g, obj2gco(tabref(L->env)));
  for (uv = gcref(L->openupval); uv != NULL; uv = gcnext(uv)) {
    lj_gc2_markobj(g, uv);
    if (uv->gch.gct == ~LJ_TUPVAL)
      gc2_mark_tv(g, uvval(gco2uv(uv)));
  }
}

static void gc2_scan_global_roots(global_State *g)
{
  ptrdiff_t i;
  lj_gc2_markobj(g, obj2gco(mainthread(g)));
  lj_gc2_markobj(g, obj2gco(tabref(mainthread(g)->env)));
  lj_gc2_markobj(g, obj2gco(vmthread(g)));
  gc2_mark_tv(g, &g->registrytv);
  for (i = 0; i < GCROOT_MAX; i++)
    if (gcref(g->gcroot[i]) != NULL)
      lj_gc2_markobj(g, gcref(g->gcroot[i]));
  gc2_mark_fixedstr(g);
  lj_gc2_markmem(g, g->str.tab);
#if LJ_64
  lj_gc2_markmem(g, mref(g->gc.lightudseg, uint32_t));
#endif
  lj_gc2_markmem(g, g->tmpbuf.b);
  {
    TGState *tg = G2TG(g);
    if (tg)
      lj_gc2_markmem(g, tg->tmpbuf.b);
  }
#if LJ_HASFFI
  {
    CTState *cts = ctype_ctsG(g);
    if (cts) {
      lj_gc2_markmem(g, cts);
      lj_gc2_markmem(g, cts->tab);
      lj_gc2_markmem(g, cts->cb.cbid);
    }
  }
#endif
#if LJ_HASJIT
  {
    jit_State *J = G2J(g);
    lj_gc2_markmem(g, J->trace);
    lj_gc2_markmem(g, J->irbuf ? J->irbuf + J->irbotlim : NULL);
    lj_gc2_markmem(g, J->snapbuf);
    lj_gc2_markmem(g, J->snapmapbuf);
  }
#endif
}

void lj_gc2_scan_roots(global_State *g, lua_State *L)
{
  if (!g)
    return;
  gc2_scan_global_roots(g);
  gc2_scan_thread_roots(g, L);
}

static void *gc2_mark_base(GCobj *o);

static void gc2_ssb_activate(TGState *tg, GC2SSBNode *node)
{
  node->next = NULL;
  node->n = 0;
  tg->ssb_active = node;
  tg->ssb_base = node->slot;
  tg->ssb_next = tg->ssb_base;
  tg->ssb_end = tg->ssb_base + TG_GC2_SSB_SLOTS;
}

static void gc2_ssb_publish(global_State *g, GC2SSBNode *node)
{
  void *head = la_loadptr_acq((void *const *)&g->gc2.ssb_head);
  do {
    node->next = (GC2SSBNode *)head;
  } while (!la_casptr((void **)&g->gc2.ssb_head, &head, node,
		      LA_ACQ_REL, LA_ACQ));  /* 05 section 5.6.2 MPSC stack. */
}

uint32_t lj_gc2_flush_ssb(global_State *g, TGState *tg)
{
  GC2SSBNode *node, *fresh;
  uint32_t n;
  if (!g || !tg || !tg->ssb_active || !tg->ssb_base || !tg->ssb_next)
    return 0;
  n = (uint32_t)(tg->ssb_next - tg->ssb_base);
  if (n == 0)
    return 0;
  fresh = tg->ssb_free;
  if (!fresh) {
    (void)lj_gc2_drain_ssb(g);  /* Temporary scaffold until workers recycle. */
    fresh = tg->ssb_free;
    if (!fresh)
      return 0;
  }
  tg->ssb_free = fresh->next;
  node = tg->ssb_active;
  node->n = n;
  gc2_ssb_publish(g, node);
  la_add32_rlx(&g->gc2.ssb_published, 1);
  la_add64_rlx(&g->gc2.ssb_items_published, n);
  gc2_ssb_activate(tg, fresh);
  return n;
}

int lj_gc2_ssb_push(global_State *g, GCobj *o)
{
  TGState *tg;
  if (!g || !o)
    return 0;
  tg = G2TG(g);
  if (!tg || !tg->ssb_next || !tg->ssb_end)
    return 0;
  if (tg->ssb_next == tg->ssb_end &&
      lj_gc2_flush_ssb(g, tg) == 0 &&
      tg->ssb_next == tg->ssb_end)
    return 0;
  setgcref(*tg->ssb_next, o);
  tg->ssb_next++;
  return 1;
}

uint32_t lj_gc2_drain_ssb(global_State *g)
{
  GC2SSBNode *node;
  uint32_t nitems = 0, nnodes = 0;
  if (!g)
    return 0;
  node = (GC2SSBNode *)la_xchgptr_acqrel((void **)&g->gc2.ssb_head, NULL);
  while (node) {
    GC2SSBNode *next = node->next;
    TGState *owner = node->owner;
    uint32_t i;
    for (i = 0; i < node->n; i++) {
      GCobj *o = gcref(node->slot[i]);
      if (o)
	(void)lj_gc2_markmem(g, gc2_mark_base(o));
    }
    nitems += node->n;
    nnodes++;
    node->n = 0;
    if (owner) {
      node->next = owner->ssb_free;
      owner->ssb_free = node;
    } else {
      node->next = NULL;
    }
    node = next;
  }
  if (nnodes) {
    la_add32_rlx(&g->gc2.ssb_drained, nnodes);
    la_add64_rlx(&g->gc2.ssb_items_drained, nitems);
  }
  return nitems;
}

int lj_gc2_ssb_empty(global_State *g)
{
  TGState *tg;
  if (!g)
    return 1;
  if (la_loadptr_acq((void *const *)&g->gc2.ssb_head) != NULL)
    return 0;  /* 05 section 5.7.1 SSB-empty fixpoint predicate. */
  for (tg = g->gc2.tg_list; tg != NULL; tg = tg->next_tg) {
    if (tg->tg_flags & TGF_DEAD)
      continue;
    if (tg->ssb_next != tg->ssb_base)
      return 0;
  }
  return 1;
}

static int gc2_mark_base_traversable(global_State *g, void *p)
{
  TGState *tg = G2TG(g);
  GCArena *a;
  if (!p || !tg || !(tg->tg_flags & TGF_ARENA_INTERNAL))
    return 0;
  a = lj_arena_of(p);
  if (lj_arena_ishuge(a)) {
    LJHugeInfo hi;
    if (!(tg->tg_flags & TGF_HUGETAB) ||
	lj_arena_hugetab_lookup(&tg->huge, p, &hi) != 1)
      return 0;
    return (hi.flags & LJ_HUGEF_TRAVERSABLE) != 0;
  }
  return (a->hdr.flags & LJ_AF_TRAVERSABLE) != 0;
}

static void *gc2_mark_base(GCobj *o)
{
#if LJ_HASFFI
  if (o->gch.gct == ~LJ_TCDATA) {
    GCcdata *cd = gco2cd(o);
    if (cdataisv(cd))
      return memcdatav(cd);
  }
#endif
  return o;
}

int lj_gc2_markmem(global_State *g, void *p)
{
  TGState *tg = G2TG(g);
  GCArena *a;
  uint32_t cell;
  int marked;
  if (!p || !tg || !(tg->tg_flags & TGF_ARENA_INTERNAL))
    return 0;
  a = lj_arena_of(p);
  if (lj_arena_ishuge(a)) {
    if (!(tg->tg_flags & TGF_HUGETAB))
      return 0;
    marked = lj_arena_hugetab_mark(&tg->huge, p, NULL);
    if (marked == 1)
      g->gc2.marks_this_round++;
    return marked == 1;
  }
  cell = lj_arena_cellof(p);
  if (cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS ||
      !lj_arena_bm_get(a->block, cell))
    return 0;
  marked = !la_bit_test_and_set64(&a->mark[cell >> 6],
				  cell & 63);  /* 05 section 5.6.1. */
  if (marked)
    g->gc2.marks_this_round++;
  return marked;
}

int lj_gc2_markobj(global_State *g, GCobj *o)
{
  void *base;
  int marked;
  if (!o)
    return 0;
  base = gc2_mark_base(o);
  marked = lj_gc2_markmem(g, base);
  if (marked && (g->gc2.phase == LJ_GC2_MARK || g->gc2.phase == LJ_GC2_WEAK) &&
      gc2_mark_base_traversable(g, base)) {
    int pushed = lj_gc2_ssb_push(g, o);  /* 05 section 5.6.1. */
    lj_assertG(pushed, "gc2 SSB push failed for marked traversable object");
    UNUSED(pushed);
  }
  return marked;
}

int lj_gc2_ismarkedmem(global_State *g, void *p)
{
  TGState *tg = G2TG(g);
  GCArena *a;
  uint32_t cell;
  if (!p || !tg || !(tg->tg_flags & TGF_ARENA_INTERNAL))
    return -1;
  a = lj_arena_of(p);
  if (lj_arena_ishuge(a)) {
    LJHugeInfo hi;
    if (!(tg->tg_flags & TGF_HUGETAB) ||
	lj_arena_hugetab_lookup(&tg->huge, p, &hi) != 1)
      return -1;
    return (hi.flags & LJ_HUGEF_MARK) != 0;
  }
  cell = lj_arena_cellof(p);
  if (cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS ||
      !lj_arena_bm_get(a->block, cell))
    return -1;
  return lj_arena_bm_get(a->mark, cell);
}

int lj_gc2_ismarked(global_State *g, GCobj *o)
{
  return o ? lj_gc2_ismarkedmem(g, gc2_mark_base(o)) : -1;
}

#if LJ_GC2_PARANOIA
static int gc2_legacy_liveobj(GCobj *o)
{
  uint8_t flags = lj_obj_gcflags(o);
  return !iswhite(o) || (flags & (LJ_GC_FIXED|LJ_GC_SFIXED));
}

static int gc2_legacy_has_base(global_State *g, void *p)
{
  GCobj *o;
  for (o = gcref(g->gc.root); o != NULL; o = gcnext(o)) {
    if (gc2_legacy_liveobj(o) && gc2_mark_base(o) == p)
      return 1;
    if (o->gch.gct == ~LJ_TTHREAD) {
      GCobj *uv;
      for (uv = gcref(gco2th(o)->openupval); uv != NULL; uv = gcnext(uv))
	if (gc2_legacy_liveobj(uv) && gc2_mark_base(uv) == p)
	  return 1;
    }
  }
  return 0;
}

static uint32_t gc2_paranoia_scan_arena(global_State *g, GCArena *a)
{
  uint32_t w, bad = 0;
  for (w = 0; w < LJ_ARENA_WORDS; w++) {
    uint64_t m = a->block[w] & a->mark[w];
    while (m) {
      uint32_t bit = (uint32_t)__builtin_ctzll(m);
      uint32_t cell = (w << 6) + bit;
      m &= m - 1u;
      if (cell >= LJ_AFIRST_CELL &&
	  !gc2_legacy_has_base(g, lj_arena_cellptr(a, cell)))
	bad++;
    }
  }
  return bad;
}

uint32_t lj_gc2_paranoia_legacy_diff(global_State *g)
{
  TGState *tg = G2TG(g);
  GCArena *a;
  uint32_t bad = 0;
  if (!tg || !(tg->tg_flags & TGF_ARENA_INTERNAL))
    return 0;
  for (a = tg->alloc.owned[LJ_ARENAK_TRAVERSABLE]; a != NULL; a = a->hdr.next)
    bad += gc2_paranoia_scan_arena(g, a);
  for (a = tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE]; a != NULL;
       a = a->hdr.next)
    bad += gc2_paranoia_scan_arena(g, a);
  return bad;
}

#endif
