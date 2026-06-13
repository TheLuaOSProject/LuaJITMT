/*
** Concurrent GC scaffold.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_gc2_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_chan.h"
#include "lj_gc2.h"
#include "lj_gc.h"
#include "lj_thr.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_meta.h"
#include "lj_safepoint.h"
#include "lj_arena.h"
#include "lj_tg.h"
#include "lj_frame.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#endif
#include "lj_trace.h"
#include "lj_mcode.h"
#include "lj_dispatch.h"

#define GC2_GREY_INIT	256u
#define GC2_GREY_LIMIT	((MSize)(LJ_MAX_MEM32 / sizeof(GCRef)))
#define GC2_WEAK_INIT	128u
#define GC2_WEAK_LIMIT	((MSize)(LJ_MAX_MEM32 / sizeof(GCRef)))

static int gc2_grey_grow(global_State *g);
static int gc2_grey_empty(global_State *g);
static void gc2_weak_reset(global_State *g);
static int gc2_tab_weak_mode(global_State *g, GCtab *t, GCtab *mt);

void lj_gc2_init(global_State *g)
{
  g->gc2.gcpause_pct = 100;
  g->gc2.assist_shift = lj_gc2_assist_shift_from_stepmul(g->gc.stepmul);
  g->gc2.phase = LJ_GC2_IDLE;
  g->gc2.cycle = 0;
  la_store32_rlx(&g->gc2.cycle_leader, 0);
  g->gc2.hs_epoch = 0;
  g->gc2.hs_pending = 0;
  g->gc2.hs_actions = 0;
  la_store64_rlx(&g->gc2.cycle_requests, 0);
  la_store64_rlx(&g->gc2.cycle_starts, 0);
  la_store64_rlx(&g->gc2.marks_this_round, 0);
  g->gc2.ssb_head = NULL;
  la_store32_rlx(&g->gc2.ssb_published, 0);
  la_store32_rlx(&g->gc2.ssb_drained, 0);
  la_store64_rlx(&g->gc2.ssb_items_published, 0);
  la_store64_rlx(&g->gc2.ssb_items_drained, 0);
  la_store64_rlx(&g->gc2.fixpoint_rounds, 0);
  la_store64_rlx(&g->gc2.fixpoint_hits, 0);
  g->gc2.alloc_since_trigger = 0;
  g->gc2.trigger_bytes = 0;
  g->gc2.hard_bytes = 0;
  la_store64_rlx(&g->gc2.assist_runs, 0);
  la_store64_rlx(&g->gc2.assist_grey_drained, 0);
  la_store64_rlx(&g->gc2.assist_ssb_converted, 0);
  la_store64_rlx(&g->gc2.jit_hard_checks, 0);
  g->gc2.assist_active = 0;
  g->gc2.grey_stack = NULL;
  g->gc2.grey_capacity = 0;
  g->gc2.grey_top = 0;
  g->gc2.grey_bottom = 0;
  la_store64_rlx(&g->gc2.grey_pushed, 0);
  la_store64_rlx(&g->gc2.grey_drained, 0);
  la_store64_rlx(&g->gc2.worker_runs, 0);
  la_store64_rlx(&g->gc2.worker_grey_drained, 0);
  la_store64_rlx(&g->gc2.worker_ssb_converted, 0);
  g->gc2.weak_stack = NULL;
  g->gc2.weak_ready = NULL;
  g->gc2.weak_capacity = 0;
  g->gc2.weak_count = 0;
  la_store64_rlx(&g->gc2.weak_tables_seen, 0);
  la_store64_rlx(&g->gc2.weak_tables_weakkey, 0);
  la_store64_rlx(&g->gc2.weak_tables_weakval, 0);
  la_store64_rlx(&g->gc2.weak_tables_allweak, 0);
  la_store64_rlx(&g->gc2.weak_tables_queued, 0);
  la_store64_rlx(&g->gc2.weak_tables_overflow, 0);
  la_store64_rlx(&g->gc2.weak_scan_cursor, 0);
  la_store64_rlx(&g->gc2.weak_scan_runs, 0);
  la_store64_rlx(&g->gc2.weak_scan_tables, 0);
  la_store64_rlx(&g->gc2.weak_scan_slots, 0);
  la_store64_rlx(&g->gc2.weak_scan_clearable, 0);
  la_store64_rlx(&g->gc2.weak_clear_cursor, 0);
  la_store64_rlx(&g->gc2.weak_clear_runs, 0);
  la_store64_rlx(&g->gc2.weak_clear_tables, 0);
  la_store64_rlx(&g->gc2.weak_clear_slots, 0);
  la_store64_rlx(&g->gc2.weak_clear_cleared, 0);
  la_store64_rlx(&g->gc2.finreg_cdata_sets, 0);
  la_store64_rlx(&g->gc2.finreg_cdata_clears, 0);
  la_store64_rlx(&g->gc2.finreg_cdata_queued, 0);
  la_store64_rlx(&g->gc2.finreg_udata_sets, 0);
  la_store64_rlx(&g->gc2.finreg_udata_clears, 0);
  la_store64_rlx(&g->gc2.finreg_udata_queued, 0);
  la_store64_rlx(&g->gc2.weak_keys_marked, 0);
  la_store64_rlx(&g->gc2.weak_values_marked, 0);
  g->gc2.tg_list = NULL;
  g->gc2.n_threads = 0;
  lj_gc2_update_pacing(g);
  lj_tg_attach(g, G2TG(g));  /* 05 section 5.4.1 main TG registration. */
}

void lj_gc2_fini(global_State *g)
{
  (void)lj_tg_reclaim_dead(g);
  if (g && g->gc2.grey_stack) {
    lj_mem_freevec(g, g->gc2.grey_stack, g->gc2.grey_capacity, GCRef);
    g->gc2.grey_stack = NULL;
    g->gc2.grey_capacity = 0;
    g->gc2.grey_top = 0;
    g->gc2.grey_bottom = 0;
  }
  if (g && g->gc2.weak_stack) {
    lj_mem_freevec(g, g->gc2.weak_stack, g->gc2.weak_capacity, GCRef);
    g->gc2.weak_stack = NULL;
  }
  if (g && g->gc2.weak_ready) {
    lj_mem_freevec(g, g->gc2.weak_ready, g->gc2.weak_capacity, uint8_t);
    g->gc2.weak_ready = NULL;
  }
  if (g) {
    g->gc2.weak_capacity = 0;
    g->gc2.weak_count = 0;
  }
}

uint64_t lj_gc2_flush_alloc(global_State *g, TGState *tg)
{
  uint64_t bytes;
  if (!g || !tg)
    return 0;
  bytes = la_xchg64_acqrel(&tg->local_total, 0);  /* 04 section 4.8. */
  if (bytes != 0)
    la_add64_rlx(&g->gc2.alloc_since_trigger, bytes);  /* 05 section 5.11. */
  return bytes;
}

static int gc2_request_cycle(global_State *g, TGState *tg)
{
  uint32_t expect = 0;
  uint32_t tid = tg ? la_load32_acq(&tg->tid) : 0;
  if (tid == 0)
    return 0;
  if (la_load32_acq(&g->gc2.phase) != LJ_GC2_IDLE)
    return 0;
  if (lj_gc_threshold_load(g) == LJ_MAX_MEM)
    return 0;  /* Honor collectgarbage("stop"). */
  if (!la_cas32(&g->gc2.cycle_leader, &expect, tid, LA_ACQ_REL, LA_ACQ))
    return 0;  /* 05 section 5.11 nonblocking cycle-request token. */
  la_add64_rlx(&g->gc2.cycle_requests, 1);  /* 05 section 5.11 telemetry. */
  lj_gc_threshold_store(g, g->gc.total);  /* Legacy cycle-driver bridge. */
  return 1;
}

static void gc2_maybe_trigger_cycle(global_State *g, TGState *tg)
{
  if (la_load32_acq(&g->gc2.phase) != LJ_GC2_IDLE)
    return;
  if (la_load64_acq(&g->gc2.alloc_since_trigger) <=
      la_load64_acq(&g->gc2.trigger_bytes))  /* 05 section 5.11 trigger. */
    return;
  (void)gc2_request_cycle(g, tg);
}

void lj_gc2_account_alloc(global_State *g, TGState *tg, GCSize bytes)
{
  uint64_t old;
  if (!g || !tg || bytes == 0)
    return;
  old = la_add64_rlx(&tg->local_total, (uint64_t)bytes);  /* 04 section 4.8. */
  if (old + (uint64_t)bytes < old || old + (uint64_t)bytes >= LJ_GC2_ACCT_FLUSH)
    (void)lj_gc2_flush_alloc(g, tg);
  gc2_maybe_trigger_cycle(g, tg);
  if (la_load64_acq(&g->gc2.alloc_since_trigger) >
      la_load64_acq(&g->gc2.hard_bytes))  /* 05 section 5.11 hard limit. */
    (void)lj_gc2_assist(g, tg);
}

uint32_t lj_gc2_assist_shift_from_stepmul(uint32_t stepmul)
{
  uint32_t shift = 0;
  uint32_t work = stepmul < 100 ? 1u : stepmul / 100u;
  while (work > 1u && shift < 8u) {
    work = (work + 1u) >> 1;
    shift++;
  }
  return shift;
}

void lj_gc2_update_pacing(global_State *g)
{
  uint64_t live, trigger, hard;
  uint32_t pct;
  if (!g)
    return;
  live = g->gc.estimate ? g->gc.estimate : g->gc.total;
  if (live < LJ_GC2_ACCT_FLUSH)
    live = LJ_GC2_ACCT_FLUSH;
  pct = la_load32_acq(&g->gc2.gcpause_pct);
  if (pct == 0)
    pct = 100;
  trigger = (live / 100u) * (uint64_t)pct +
	    ((live % 100u) * (uint64_t)pct) / 100u;
  if (trigger < LJ_GC2_ACCT_FLUSH)
    trigger = LJ_GC2_ACCT_FLUSH;
  hard = trigger > ~(uint64_t)0 / 2u ? ~(uint64_t)0 : trigger * 2u;
  la_store64_rel(&g->gc2.trigger_bytes, trigger);  /* 05 section 5.11. */
  la_store64_rel(&g->gc2.hard_bytes, hard);  /* 05 section 5.11. */
}

static void gc2_reset_alloc_trigger(global_State *g)
{
  TGState *tg;
  for (tg = (TGState *)la_loadptr_acq((void *const *)&g->gc2.tg_list);
       tg != NULL;
       tg = (TGState *)la_loadptr_acq((void *const *)&tg->next_tg))
    (void)lj_gc2_flush_alloc(g, tg);
  la_store64_rlx(&g->gc2.alloc_since_trigger, 0);  /* 05 section 5.11. */
}

static TGState *gc2_tg_for_mem(global_State *g, const void *p)
{
  if (p) {
    uint32_t owner_tid = lj_arena_of(p)->hdr.owner_tid;
    TGState *owner = lj_tg_find_owner(g, owner_tid);
    if (owner)
      return owner;
  }
  return G2TG(g);
}

static void gc2_clear_marks(TGState *tg)
{
  if (tg && (tg->tg_flags & TGF_ARENA_INTERNAL)) {
    lj_arena_alloc_clear_marks(&tg->alloc);
    if (tg->tg_flags & TGF_HUGETAB)
      lj_arena_hugetab_clear_marks(&tg->huge);
  }
}

static void gc2_clear_marks_all(global_State *g)
{
  TGState *tg;
  for (tg = (TGState *)la_loadptr_acq((void *const *)&g->gc2.tg_list);
       tg != NULL;
       tg = (TGState *)la_loadptr_acq((void *const *)&tg->next_tg))
    gc2_clear_marks(tg);
}

static void gc2_mark_strtab_mem(global_State *g)
{
  StrTabHdr *hdr;
  hdr = (StrTabHdr *)la_loadptr_acq((void *const *)&g->str.tabh);
  if (hdr)
    lj_gc2_markmem(g, hdr);
  for (hdr = (StrTabHdr *)la_loadptr_acq((void *const *)&g->str.retired);
       hdr != NULL;
       hdr = (StrTabHdr *)la_loadptr_acq((void *const *)&hdr->retired_next))
    lj_gc2_markmem(g, hdr);
}

static void gc2_mark_tab_retired_mem(global_State *g)
{
  TabNodeRetire *ret;
  for (ret = (TabNodeRetire *)la_loadptr_acq(
	 (void *const *)&g->tab.retired_nodes);
       ret != NULL;
       ret = (TabNodeRetire *)la_loadptr_acq((void *const *)&ret->next)) {
    lj_gc2_markmem(g, ret);
    if (la_load32_acq(&ret->armed))
      lj_gc2_markmem(g, lj_tab_node_hdrw(ret->node));
  }
}

void lj_gc2_legacy_mark_begin(global_State *g)
{
  TGState *tg = G2TG(g);
  uint32_t leader;
  /* Publish MARK before clearing the request token, so late allocators stop. */
  g->gc2.phase = LJ_GC2_MARK;
  leader = la_xchg32_acqrel(&g->gc2.cycle_leader, 0);
  if (g->gc2.tg_list == NULL && tg != NULL)
    lj_tg_attach(g, tg);
  g->gc2.cycle++;
  if (leader)
    la_add64_rlx(&g->gc2.cycle_starts, 1);
  la_store64_rlx(&g->gc2.marks_this_round, 0);
  (void)lj_gc2_drain_ssb(g);  /* Finish prior-cycle scaffold work. */
  (void)lj_tg_reclaim_dead(g);
  lj_assertG(gc2_grey_empty(g), "gc2 grey deque not empty at mark begin");
  la_store64_rlx(&g->gc2.grey_top, 0);
  la_store64_rlx(&g->gc2.grey_bottom, 0);
  if (g->gc2.grey_capacity == 0)
    (void)gc2_grey_grow(g);
  gc2_weak_reset(g);
  gc2_reset_alloc_trigger(g);
  gc2_clear_marks_all(g);
  lj_gc2_handshake(g, LJ_GC2_HS_ENABLE_BARRIER|LJ_GC2_HS_ALLOC_BLACK);
}

void lj_gc2_legacy_weak_begin(global_State *g)
{
  g->gc2.phase = LJ_GC2_WEAK;  /* 05 section 5.8 legacy weak-phase bridge. */
}

void lj_gc2_legacy_sweep_begin(global_State *g)
{
  g->gc2.phase = LJ_GC2_SWEEP;
  lj_gc2_handshake(g, LJ_GC2_HS_DISABLE_BARRIER|LJ_GC2_HS_RESET_ALLOC|
		   LJ_GC2_HS_FLUSH_SSB);
  (void)lj_gc2_drain_ssb(g);  /* Temporary worker-consume stand-in. */
  (void)lj_tg_reclaim_dead(g);
}

void lj_gc2_legacy_preserve_abort(global_State *g)
{
  la_store32_rel(&g->gc2.cycle_leader, 0);
  g->gc2.phase = LJ_GC2_IDLE;
  lj_gc2_handshake(g, LJ_GC2_HS_DISABLE_BARRIER|LJ_GC2_HS_ALLOC_WHITE|
		   LJ_GC2_HS_FLUSH_SSB);
  (void)lj_gc2_drain_ssb(g);
  (void)lj_tg_reclaim_dead(g);
}

void lj_gc2_legacy_cycle_end(global_State *g)
{
  la_store32_rel(&g->gc2.cycle_leader, 0);
  g->gc2.phase = LJ_GC2_IDLE;
  lj_gc2_handshake(g, LJ_GC2_HS_DISABLE_BARRIER|LJ_GC2_HS_ALLOC_WHITE|
		   LJ_GC2_HS_FLUSH_SSB);
  (void)lj_gc2_drain_ssb(g);
  (void)lj_tg_reclaim_dead(g);
  lj_gc2_update_pacing(g);
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
  TValue tv;
  if (!L || tvref(L->stack) == NULL)
    return;
  lj_gc2_markobj(g, obj2gco(L));
  lj_gc2_markmem(g, tvref(L->stack));
  top = gc2_stack_scan_top(g, L);
  for (o = tvref(L->stack) + 1 + LJ_FR2; o < top; o++) {
    lj_tv_load_acq(&tv, o);
    gc2_mark_tv(g, &tv);
  }
  {
    GCtab *env = tabref_acq(L->env);
    if (env)
      lj_gc2_markobj(g, obj2gco(env));
  }
  for (uv = gcref(L->openupval); uv != NULL; uv = gcnext(uv)) {
    lj_gc2_markobj(g, uv);
    if (uv->gch.gct == ~LJ_TUPVAL) {
      TValue tv;
      lj_tv_load_acq(&tv, uvval(gco2uv(uv)));
      gc2_mark_tv(g, &tv);
    }
  }
}

static void gc2_scan_global_roots(global_State *g)
{
  ptrdiff_t i;
  lj_gc2_markobj(g, obj2gco(mainthread(g)));
  {
    GCtab *env = tabref_acq(mainthread(g)->env);
    if (env)
      lj_gc2_markobj(g, obj2gco(env));
  }
  lj_gc2_markobj(g, obj2gco(vmthread(g)));
  gc2_mark_tv(g, &g->registrytv);
  for (i = 0; i < GCROOT_MAX; i++) {
    GCobj *o = gcref_acq(g->gcroot[i]);
    if (o != NULL)
      lj_gc2_markobj(g, o);
  }
  {
    LJThreadLive *node;
    for (node = (LJThreadLive *)
	   la_loadptr_acq((void *const *)&g->threading_live);
	 node != NULL;
	 node = (LJThreadLive *)
	   la_loadptr_acq((void *const *)&node->next)) {
      GCobj *o = gcref_acq(node->ud);
      if (o && o->gch.gct == ~LJ_TUDATA &&
	  lj_udata_udtype_acq(gco2ud(o)) == UDTYPE_THREAD)
	lj_gc2_markobj(g, o);
    }
  }
  gc2_mark_fixedstr(g);
  gc2_mark_strtab_mem(g);
  gc2_mark_tab_retired_mem(g);
#if LJ_64
  lj_gc2_markmem(g, mref(g->gc.lightudseg, uint32_t));
#endif
  lj_gc2_markmem(g, g->tmpbuf.b);
  {
    TGState *tg;
    for (tg = (TGState *)la_loadptr_acq((void *const *)&g->gc2.tg_list);
	 tg != NULL;
	 tg = (TGState *)la_loadptr_acq((void *const *)&tg->next_tg))
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
    lj_trace_markvecs(g, 1);
    lj_mcode_markretired(g, 1);
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
static int gc2_mark_base_traversable(global_State *g, void *p);
static int gc2_grey_push(global_State *g, GCobj *o);
static uint32_t gc2_drain_grey(global_State *g, uint32_t limit);
static void gc2_traverse_udata(global_State *g, GCudata *ud);

static int gc2_grey_grow(global_State *g)
{
  GCRef *oldstack = g->gc2.grey_stack;
  MSize oldcap = g->gc2.grey_capacity;
  MSize newcap = oldcap ? oldcap << 1 : GC2_GREY_INIT;
  uint64_t top = la_load64_acq(&g->gc2.grey_top);
  uint64_t bottom = la_load64_rlx(&g->gc2.grey_bottom);
  MSize count = bottom > top ? (MSize)(bottom - top) : 0;
  lua_State *L = lj_tg_cur_L(g);
  if (!L && gcref(g->mainthref))
    L = mainthread(g);
  if (!L || oldcap >= GC2_GREY_LIMIT)
    return 0;
  if (newcap < oldcap || newcap > GC2_GREY_LIMIT)
    newcap = GC2_GREY_LIMIT;
  if (newcap <= oldcap || count > newcap)
    return 0;
  g->gc2.grey_stack = lj_mem_newvec(L, newcap, GCRef);
  if (oldstack && oldcap) {
    MSize i;
    for (i = 0; i < count; i++)
      g->gc2.grey_stack[i] = oldstack[(MSize)((top + i) % oldcap)];
    lj_mem_freevec(g, oldstack, oldcap, GCRef);
  }
  g->gc2.grey_capacity = newcap;
  la_store64_rlx(&g->gc2.grey_top, 0);
  la_store64_rlx(&g->gc2.grey_bottom, count);
  return 1;
}

static int gc2_grey_push(global_State *g, GCobj *o)
{
  uint64_t top, bottom;
  MSize cap;
  if (!g || !o)
    return 0;
  bottom = la_load64_rlx(&g->gc2.grey_bottom);
  top = la_load64_acq(&g->gc2.grey_top);
  cap = g->gc2.grey_capacity;
  if ((cap == 0 || bottom - top >= cap) && !gc2_grey_grow(g))
    return 0;
  bottom = la_load64_rlx(&g->gc2.grey_bottom);
  cap = g->gc2.grey_capacity;
  setgcref(g->gc2.grey_stack[(MSize)(bottom % cap)], o);
  la_fence_rel();  /* 05 section 5.6.3: publish slot before bottom. */
  la_store64_rel(&g->gc2.grey_bottom, bottom + 1);
  la_add64_rlx(&g->gc2.grey_pushed, 1);
  return 1;
}

static int gc2_grey_empty(global_State *g)
{
  if (!g)
    return 1;
  return la_load64_acq(&g->gc2.grey_top) ==
	 la_load64_acq(&g->gc2.grey_bottom);
}

static int gc2_weak_ensure(global_State *g)
{
  lua_State *L;
  if (!g)
    return 0;
  if (g->gc2.weak_stack && g->gc2.weak_ready && g->gc2.weak_capacity > 0)
    return 1;
  L = mainthread(g);
  if (!L)
    return 0;
  if (!g->gc2.weak_stack)
    g->gc2.weak_stack = lj_mem_newvec(L, GC2_WEAK_INIT, GCRef);
  if (!g->gc2.weak_ready)
    g->gc2.weak_ready = lj_mem_newvec(L, GC2_WEAK_INIT, uint8_t);
  g->gc2.weak_capacity = GC2_WEAK_INIT;
  return 1;
}

static void gc2_weak_reset(global_State *g)
{
  MSize i;
  if (!g)
    return;
  (void)gc2_weak_ensure(g);
  for (i = 0; i < g->gc2.weak_capacity; i++)
    la_store8_rlx(&g->gc2.weak_ready[i], 0);
  la_store64_rlx(&g->gc2.weak_count, 0);  /* 05 section 5.8 side vector. */
  la_store64_rlx(&g->gc2.weak_scan_cursor, 0);
  la_store64_rlx(&g->gc2.weak_clear_cursor, 0);
}

static void gc2_weak_record(global_State *g, GCtab *t)
{
  uint64_t idx;
  if (!g || !t || !g->gc2.weak_stack || !g->gc2.weak_ready ||
      g->gc2.weak_capacity == 0) {
    if (g)
      la_add64_rlx(&g->gc2.weak_tables_overflow, 1);
    return;
  }
  idx = la_add64_rlx(&g->gc2.weak_count, 1);  /* 05 section 5.8 MPSC slot. */
  if (idx < g->gc2.weak_capacity) {
    setgcref(g->gc2.weak_stack[(MSize)idx], obj2gco(t));
    /* 05 section 5.8: publish weak snapshot slot before ready byte. */
    la_store8_rel(&g->gc2.weak_ready[(MSize)idx], 1);
    la_add64_rlx(&g->gc2.weak_tables_queued, 1);
  } else {
    la_add64_rlx(&g->gc2.weak_tables_overflow, 1);
  }
}

uint32_t lj_gc2_weak_snapshot_count(global_State *g)
{
  uint64_t reserved, count;
  MSize cap;
  if (!g || !g->gc2.weak_stack || !g->gc2.weak_ready)
    return 0;
  reserved = la_load64_acq(&g->gc2.weak_count);
  cap = g->gc2.weak_capacity;
  if (reserved > (uint64_t)cap)
    reserved = (uint64_t)cap;
  for (count = 0; count < reserved; count++)
    if (la_load8_acq(&g->gc2.weak_ready[(MSize)count]) == 0)
      break;
  return count > ~(uint32_t)0 ? ~(uint32_t)0 : (uint32_t)count;
}

GCtab *lj_gc2_weak_snapshot_tab(global_State *g, uint32_t idx)
{
  GCobj *o;
  if (!g || !g->gc2.weak_stack || idx >= lj_gc2_weak_snapshot_count(g))
    return NULL;
  o = gcref(g->gc2.weak_stack[idx]);
  return (o && o->gch.gct == ~LJ_TTAB) ? gco2tab(o) : NULL;
}

static int gc2_weak_mayclear(global_State *g, cTValue *o, int val)
{
  if (tvisgcv(o)) {
    if (tvisstr(o))
      return 0;  /* 05 section 5.8: strings are not weak-cleared. */
    if (lj_gc2_ismarked(g, gcV(o)) == 0)
      return 1;
    if (tvisudata(o) && val &&
	(lj_obj_gcflags(obj2gco(udataV(o))) & LJ_GC_FINALIZED))
      return 1;
  }
  return 0;
}

static int gc2_tab_is_ffi_fin(global_State *g, GCtab *t)
{
#if LJ_HASFFI
  return gcref_acq(g->gcroot[GCROOT_FFI_FIN]) == obj2gco(t);
#else
  UNUSED(g); UNUSED(t);
  return 0;
#endif
}

static void gc2_weak_process_tab(global_State *g, GCtab *t, int clear,
				 uint64_t *slots, uint64_t *clearable)
{
  GCtab *mt = tabref_acq(t->metatable);
  int weak = gc2_tab_weak_mode(g, t, mt);
  if (!weak)
    return;
  if (weak & LJ_GC_WEAKVAL) {
    MSize i, asize = lj_tab_asize_acq(t);
    TValue *array = lj_tab_array_acq(t);
    for (i = 0; i < asize; i++) {
      TValue val;
      lj_tv_load_acq(&val, &array[i]);
      if (!tvisnil(&val)) {
	(*slots)++;
	if (gc2_weak_mayclear(g, &val, 1)) {
	  (*clearable)++;
	  if (clear)
	    lj_tab_storenilraw(&array[i]);
	}
      }
    }
  }
  {
    Node *node = lj_tab_node_acq(t);
    MSize i, hmask = lj_tab_node_hmask_acq(node);
    if (hmask > 0) {
      for (i = 0; i <= hmask; i++) {
	Node *n = &node[i];
	TValue key, val;
	lj_tv_load_acq(&val, &n->val);
	if (!tvisnil(&val)) {
	  lj_tv_load_acq(&key, &n->key);
	  (*slots)++;
	  if (gc2_weak_mayclear(g, &key, 0) ||
	      gc2_weak_mayclear(g, &val, 1)) {
	    if (!clear) {
	      (*clearable)++;
	    } else if (!tvisstr(&key) && !tvisstr(&val)) {
	      (*clearable)++;
	      lj_tab_storenilraw(&n->val);
	    }
	  }
	}
      }
    }
  }
}

uint32_t lj_gc2_weak_snapshot_scan(global_State *g, uint32_t limit)
{
  uint64_t start, end;
  uint32_t i, n, scanned = 0;
  uint64_t slots = 0, clearable = 0;
  if (!g || limit == 0)
    return 0;
  n = lj_gc2_weak_snapshot_count(g);
  do {
    start = la_load64_acq(&g->gc2.weak_scan_cursor);
    if (start >= (uint64_t)n)
      return 0;
    end = start + limit;
    if (end < start || end > (uint64_t)n)
      end = (uint64_t)n;
  } while (!la_cas64(&g->gc2.weak_scan_cursor, &start, end,
		     LA_ACQ_REL, LA_ACQ));  /* 05 section 5.8 bounded scan cursor. */
  for (i = (uint32_t)start; (uint64_t)i < end; i++) {
    GCtab *t = lj_gc2_weak_snapshot_tab(g, i);
    if (!t)
      continue;
    gc2_weak_process_tab(g, t, 0, &slots, &clearable);
    scanned++;
  }
  if (scanned) {
    la_add64_rlx(&g->gc2.weak_scan_runs, 1);
    la_add64_rlx(&g->gc2.weak_scan_tables, scanned);
    la_add64_rlx(&g->gc2.weak_scan_slots, slots);
    la_add64_rlx(&g->gc2.weak_scan_clearable, clearable);
  }
  return scanned;
}

uint32_t lj_gc2_weak_snapshot_clear(global_State *g, uint32_t limit)
{
  uint64_t start, end;
  uint32_t i, n, scanned = 0;
  uint64_t slots = 0, cleared = 0;
  if (!g || limit == 0)
    return 0;
  n = lj_gc2_weak_snapshot_count(g);
  do {
    start = la_load64_acq(&g->gc2.weak_clear_cursor);
    if (start >= (uint64_t)n)
      return 0;
    end = start + limit;
    if (end < start || end > (uint64_t)n)
      end = (uint64_t)n;
  } while (!la_cas64(&g->gc2.weak_clear_cursor, &start, end,
		     LA_ACQ_REL, LA_ACQ));  /* 05 section 5.8 bounded clear cursor. */
  for (i = (uint32_t)start; (uint64_t)i < end; i++) {
    GCtab *t = lj_gc2_weak_snapshot_tab(g, i);
    if (!t)
      continue;
    gc2_weak_process_tab(g, t, 1, &slots, &cleared);
    scanned++;
  }
  if (scanned) {
    la_add64_rlx(&g->gc2.weak_clear_runs, 1);
    la_add64_rlx(&g->gc2.weak_clear_tables, scanned);
    la_add64_rlx(&g->gc2.weak_clear_slots, slots);
    la_add64_rlx(&g->gc2.weak_clear_cleared, cleared);
  }
  return scanned;
}

uint32_t lj_gc2_weak_drain(global_State *g, uint32_t limit)
{
  if (!g || limit == 0 || g->gc2.phase != LJ_GC2_WEAK)
    return 0;
  return lj_gc2_weak_snapshot_clear(g, limit);
}

void lj_gc2_finreg_cdata_set(global_State *g, GCobj *o, int enabled)
{
#if LJ_HASFFI
  if (!g || !o || o->gch.gct != ~LJ_TCDATA)
    return;
  if (enabled)
    la_add64_rlx(&g->gc2.finreg_cdata_sets, 1);
  else
    la_add64_rlx(&g->gc2.finreg_cdata_clears, 1);
#else
  UNUSED(g); UNUSED(o); UNUSED(enabled);
#endif
}

void lj_gc2_finreg_cdata_queue(global_State *g, GCobj *o)
{
#if LJ_HASFFI
  if (!g || !o || o->gch.gct != ~LJ_TCDATA)
    return;
  la_add64_rlx(&g->gc2.finreg_cdata_queued, 1);
#else
  UNUSED(g); UNUSED(o);
#endif
}

void lj_gc2_finreg_udata_set(global_State *g, GCobj *o, int enabled)
{
  if (!g || !o || o->gch.gct != ~LJ_TUDATA)
    return;
  if (enabled)
    la_add64_rlx(&g->gc2.finreg_udata_sets, 1);
  else
    la_add64_rlx(&g->gc2.finreg_udata_clears, 1);
}

void lj_gc2_finreg_udata_queue(global_State *g, GCobj *o)
{
  if (!g || !o || o->gch.gct != ~LJ_TUDATA)
    return;
  la_add64_rlx(&g->gc2.finreg_udata_queued, 1);
}

static GCobj *gc2_grey_pop(global_State *g)
{
  uint64_t top, bottom;
  GCobj *o;
  MSize cap;
  if (!g || !g->gc2.grey_stack || g->gc2.grey_capacity == 0)
    return NULL;
  bottom = la_load64_rlx(&g->gc2.grey_bottom);
  if (bottom == 0)
    return NULL;
  bottom--;
  la_store64_rlx(&g->gc2.grey_bottom, bottom);
  la_fence_seq();  /* Chase-Lev owner pop: order bottom before top load. */
  top = la_load64_acq(&g->gc2.grey_top);
  if (top <= bottom) {
    cap = g->gc2.grey_capacity;
    o = gcref(g->gc2.grey_stack[(MSize)(bottom % cap)]);
    if (top == bottom) {
      uint64_t expect = top;
      if (!la_cas64(&g->gc2.grey_top, &expect, top + 1,
		    LA_SEQ, LA_ACQ)) {  /* 05 section 5.6.3 single item. */
	o = NULL;
      }
      la_store64_rel(&g->gc2.grey_bottom, top + 1);
    }
    return o;
  }
  la_store64_rel(&g->gc2.grey_bottom, top);
  return NULL;
}

GCobj *lj_gc2_grey_steal(global_State *g)
{
  uint64_t top, bottom, expect;
  GCobj *o;
  MSize cap;
  if (!g || !g->gc2.grey_stack || g->gc2.grey_capacity == 0)
    return NULL;
  /* 05 section 5.6.3: non-owner steal; deque growth is owner-quiesced. */
  top = la_load64_acq(&g->gc2.grey_top);
  la_fence_seq();  /* 05 section 5.6.3: order top before bottom snapshot. */
  bottom = la_load64_acq(&g->gc2.grey_bottom);
  if (top >= bottom)
    return NULL;
  cap = g->gc2.grey_capacity;
  o = gcref(g->gc2.grey_stack[(MSize)(top % cap)]);
  expect = top;
  if (!la_cas64(&g->gc2.grey_top, &expect, top + 1,
		LA_SEQ, LA_ACQ))  /* 05 section 5.6.3 steal claim. */
    return NULL;
  return o;
}

static void gc2_ssb_activate(TGState *tg, GC2SSBNode *node)
{
  node->next = NULL;
  node->n = 0;
  tg->ssb_active = node;
  /* 05 section 5.6.2: publish active SSB cursor reset. */
  la_storeptr_rel((void **)&tg->ssb_base, node->slot);
  la_storeptr_rel((void **)&tg->ssb_end,
		  node->slot + TG_GC2_SSB_SLOTS);
  la_storeptr_rel((void **)&tg->ssb_next, node->slot);
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
  GCRef *base, *next;
  uint32_t n;
  if (!g || !tg || !tg->ssb_active)
    return 0;
  base = (GCRef *)la_loadptr_acq((void *const *)&tg->ssb_base);
  next = (GCRef *)la_loadptr_acq((void *const *)&tg->ssb_next);
  if (!base || !next)
    return 0;
  n = (uint32_t)(next - base);
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
  GCRef *next, *end;
  if (!g || !o)
    return 0;
  tg = G2TG(g);
  if (!tg)
    return 0;
  next = (GCRef *)la_loadptr_acq((void *const *)&tg->ssb_next);
  end = (GCRef *)la_loadptr_acq((void *const *)&tg->ssb_end);
  if (!next || !end)
    return 0;
  if (next == end) {
    if (lj_gc2_flush_ssb(g, tg) == 0)
      return 0;
    next = (GCRef *)la_loadptr_acq((void *const *)&tg->ssb_next);
    end = (GCRef *)la_loadptr_acq((void *const *)&tg->ssb_end);
    if (!next || !end || next == end)
      return 0;
  }
  setgcref(*next, o);
  /* 05 section 5.6.2: publish slot before cursor advance. */
  la_storeptr_rel((void **)&tg->ssb_next, next + 1);
  return 1;
}

static void gc2_ssb_mark_one(global_State *g, GCobj *o)
{
  if (o) {
    void *base = gc2_mark_base(o);
    (void)lj_gc2_markmem(g, base);
    if (gc2_mark_base_traversable(g, base)) {
      int pushed = gc2_grey_push(g, o);
      lj_assertG(pushed, "gc2 grey push failed for SSB object");
      UNUSED(pushed);
    }
  }
}

static void gc2_ssb_recycle_node(GC2SSBNode *node)
{
  TGState *owner = node->owner;
  node->n = 0;
  if (owner) {
    node->next = owner->ssb_free;
    owner->ssb_free = node;
  } else {
    node->next = NULL;
  }
}

static void gc2_ssb_publish_list(global_State *g, GC2SSBNode *head)
{
  GC2SSBNode *tail;
  void *oldhead;
  if (!g || !head)
    return;
  tail = head;
  while (tail->next)
    tail = tail->next;
  oldhead = la_loadptr_acq((void *const *)&g->gc2.ssb_head);
  do {
    tail->next = (GC2SSBNode *)oldhead;
  } while (!la_casptr((void **)&g->gc2.ssb_head, &oldhead, head,
		      LA_ACQ_REL, LA_ACQ));  /* 05 section 5.6.2 partial drain. */
}

static LJ_NOINLINE uint32_t gc2_drain_published_ssb_to_grey(global_State *g,
							    uint32_t limit)
{
  GC2SSBNode *node;
  uint32_t nitems = 0, nnodes = 0;
  if (!g || limit == 0)
    return 0;
  node = (GC2SSBNode *)la_xchgptr_acqrel((void **)&g->gc2.ssb_head, NULL);
  while (node && nitems < limit) {
    GC2SSBNode *next = node->next;
    while (node->n > 0 && nitems < limit) {
      GCRef *slot = &node->slot[node->n - 1u];
      GCobj *o = gcref(*slot);
      setgcrefnull(*slot);
      node->n--;
      gc2_ssb_mark_one(g, o);
      nitems++;
    }
    if (node->n == 0) {
      nnodes++;
      gc2_ssb_recycle_node(node);
      node = next;
    } else {
      node->next = next;
      break;
    }
  }
  if (node)
    gc2_ssb_publish_list(g, node);
  if (nnodes) {
    la_add32_rlx(&g->gc2.ssb_drained, nnodes);
  }
  if (nitems)
    la_add64_rlx(&g->gc2.ssb_items_drained, nitems);
  return nitems;
}

static uint32_t gc2_drain_active_ssb_to_grey(global_State *g, TGState *tg,
					     uint32_t limit)
{
  GCRef *base, *next;
  uint32_t n = 0;
  if (!g || !tg || limit == 0)
    return 0;
  base = (GCRef *)la_loadptr_acq((void *const *)&tg->ssb_base);
  next = (GCRef *)la_loadptr_acq((void *const *)&tg->ssb_next);
  if (!base || !next)
    return 0;
  while (n < limit && next > base) {
    GCRef *slot = next - 1;
    GCobj *o = gcref(*slot);
    setgcrefnull(*slot);
    gc2_ssb_mark_one(g, o);
    next = slot;
    /* 05 section 5.7.1: publish slot processed before cursor retreat. */
    la_storeptr_rel((void **)&tg->ssb_next, next);
    n++;
  }
  return n;
}

uint32_t lj_gc2_drain_ssb(global_State *g)
{
  uint32_t nitems;
  if (!g)
    return 0;
  nitems = gc2_drain_published_ssb_to_grey(g, ~(uint32_t)0);
  (void)gc2_drain_grey(g, ~(uint32_t)0);  /* Temporary single-worker scaffold. */
  return nitems;
}

uint32_t lj_gc2_assist(global_State *g, TGState *tg)
{
  uint32_t phase, shift, limit, expect = 0, n = 0, converted = 0;
  if (!g || !tg || tg->gc_assist)
    return 0;
  phase = la_load32_acq(&g->gc2.phase);
  if (phase != LJ_GC2_MARK && phase != LJ_GC2_WEAK)
    return 0;
  if (la_load64_acq(&g->gc2.alloc_since_trigger) <=
      la_load64_acq(&g->gc2.hard_bytes))
    return 0;
  if (!la_cas32(&g->gc2.assist_active, &expect, 1, LA_ACQ_REL, LA_ACQ))
    return 0;  /* Current global grey deque has one owner side. */
  tg->gc_assist = 1;
  la_add64_rlx(&g->gc2.assist_runs, 1);  /* 05 section 5.11 telemetry. */
  shift = la_load32_acq(&g->gc2.assist_shift);
  if (shift > 8u)
    shift = 8u;
  limit = 1u << shift;
  (void)lj_gc2_flush_alloc(g, tg);
  while (n < limit) {
    uint32_t left = limit - n;
    uint32_t drained = gc2_drain_grey(g, left);
    if (drained) {
      n += drained;
      continue;
    }
    if (converted >= limit)
      break;
    if (!gc2_drain_active_ssb_to_grey(g, tg, 1) &&
	!gc2_drain_published_ssb_to_grey(g, 1))
      break;
    converted++;
  }
  if (n)
    la_add64_rlx(&g->gc2.assist_grey_drained, n);
  if (converted)
    la_add64_rlx(&g->gc2.assist_ssb_converted, converted);
  tg->gc_assist = 0;
  la_store32_rel(&g->gc2.assist_active, 0);
  return n;
}

int lj_gc2_ssb_empty(global_State *g)
{
  TGState *tg;
  if (!g)
    return 1;
  if (la_loadptr_acq((void *const *)&g->gc2.ssb_head) != NULL)
    return 0;  /* 05 section 5.7.1 SSB-empty fixpoint predicate. */
  if (!gc2_grey_empty(g))
    return 0;
  for (tg = (TGState *)la_loadptr_acq((void *const *)&g->gc2.tg_list);
       tg != NULL;
       tg = (TGState *)la_loadptr_acq((void *const *)&tg->next_tg)) {
    GCRef *next, *base;
    if (la_load8_acq(&tg->tg_flags) & TGF_DEAD)
      continue;
    next = (GCRef *)la_loadptr_acq((void *const *)&tg->ssb_next);
    base = (GCRef *)la_loadptr_acq((void *const *)&tg->ssb_base);
    if (next != base)
      return 0;
  }
  return 1;
}

static int gc2_barrier_active_g(global_State *g)
{
  TGState *tg;
  if (!g)
    return 0;
  tg = G2TG(g);
  if (!tg || !tg->mark_active)
    return 0;
  if (g->gc2.phase != LJ_GC2_MARK && g->gc2.phase != LJ_GC2_WEAK)
    return 0;
  return 1;
}

static int gc2_barrier_active(lua_State *L, global_State **pg)
{
  global_State *g;
  if (!L)
    return 0;
  g = G(L);
  if (!gc2_barrier_active_g(g))
    return 0;
  *pg = g;
  return 1;
}

static int gc2_tab_weak_mode(global_State *g, GCtab *t, GCtab *mt)
{
  int weak = 0;
  TValue modev;
  cTValue *mode = lj_meta_fasttv(g, mt, MM_mode, &modev);
  if (mode && tvisstr(mode)) {
    const char *modestr = strVdata(mode);
    int c;
    while ((c = *modestr++)) {
      if (c == 'k') weak |= LJ_GC_WEAKKEY;
      else if (c == 'v') weak |= LJ_GC_WEAKVAL;
    }
  #if LJ_HASFFI
    if (weak && gc2_tab_is_ffi_fin(g, t))
      weak = (int)(~0u & ~LJ_GC_WEAKVAL);
  #endif
  }
  return weak;
}

void lj_gc2_barrier_tv(lua_State *L, cTValue *tv)
{
  global_State *g;
  TValue snap;
  if (tv) {
    lj_tv_load_acq(&snap, tv);
    if (tvisgcv(&snap) && gc2_barrier_active(L, &g))
      lj_gc2_markobj(g, gcV(&snap));
  }
}

void lj_gc2_barrier_tv_g(global_State *g, cTValue *tv)
{
  TValue snap;
  if (tv) {
    lj_tv_load_acq(&snap, tv);
    if (tvisgcv(&snap) && gc2_barrier_active_g(g))
      lj_gc2_markobj(g, gcV(&snap));
  }
}

void lj_gc2_barrier_tvn_g(global_State *g, cTValue *tv, uint32_t n)
{
  uint32_t i;
  if (!tv || !gc2_barrier_active_g(g))
    return;
  for (i = 0; i < n; i++)
    lj_gc2_barrier_tv_g(g, &tv[i]);
}

void lj_gc2_barrier_uv(global_State *g, cTValue *tv)
{
  lj_gc2_barrier_tv_g(g, tv);
}

void lj_gc2_barrier_obj(lua_State *L, GCobj *o)
{
  global_State *g;
  if (o && gc2_barrier_active(L, &g))
    lj_gc2_markobj(g, o);
}

static void gc2_barrier_tab_mark(global_State *g, GCtab *t)
{
  GCobj *o;
  int marked;
  o = obj2gco(t);
  marked = lj_gc2_ismarked(g, o);
  if (marked > 0) {
    int pushed = lj_gc2_ssb_push(g, o);
    lj_assertG(pushed, "gc2 table barrier SSB push failed");
    UNUSED(pushed);
  } else if (marked == 0) {
    (void)lj_gc2_markobj(g, o);
  }
}

void lj_gc2_barrier_tab_g(global_State *g, GCtab *t)
{
  if (t && gc2_barrier_active_g(g))
    gc2_barrier_tab_mark(g, t);
}

void lj_gc2_barrier_tab(lua_State *L, GCtab *t)
{
  global_State *g;
  if (t && gc2_barrier_active(L, &g))
    gc2_barrier_tab_mark(g, t);
}

void lj_gc2_barrier_weak_key(lua_State *L, GCtab *t, cTValue *key)
{
  global_State *g;
  GCtab *mt;
  if (!L || !t || !key || !tvisgcv(key))
    return;
  g = G(L);
  if (la_load32_acq(&g->gc2.phase) != LJ_GC2_WEAK)
    return;
  mt = tabref_acq(t->metatable);
  if ((gc2_tab_weak_mode(g, t, mt) & LJ_GC_WEAKKEY) &&
      lj_gc2_markobj(g, gcV(key)))  /* 05 section 5.8 weak-key write. */
    la_add64_rlx(&g->gc2.weak_keys_marked, 1);
}

void lj_gc2_barrier_weak_write(lua_State *L, GCtab *t, cTValue *key,
			       cTValue *val)
{
  global_State *g;
  GCtab *mt;
  if (!L || !t)
    return;
  g = G(L);
  if (la_load32_acq(&g->gc2.phase) != LJ_GC2_WEAK)
    return;
  mt = tabref_acq(t->metatable);
  if (gc2_tab_weak_mode(g, t, mt) == 0)
    return;
  if (key && tvisgcv(key) && lj_gc2_markobj(g, gcV(key)))
    la_add64_rlx(&g->gc2.weak_keys_marked, 1);
  if (val && tvisgcv(val) && lj_gc2_markobj(g, gcV(val)))
    la_add64_rlx(&g->gc2.weak_values_marked, 1);
}

static int gc2_mark_base_traversable(global_State *g, void *p)
{
  TGState *tg = gc2_tg_for_mem(g, p);
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
  TGState *tg = gc2_tg_for_mem(g, p);
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
      la_add64_rlx(&g->gc2.marks_this_round, 1);  /* 05 section 5.7.1. */
    return marked == 1;
  }
  cell = lj_arena_cellof(p);
  if (cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS ||
      !lj_arena_bm_get(a->block, cell))
    return 0;
  marked = !la_bit_test_and_set64(&a->mark[cell >> 6],
				  cell & 63);  /* 05 section 5.6.1. */
  if (marked)
    la_add64_rlx(&g->gc2.marks_this_round, 1);  /* 05 section 5.7.1. */
  return marked;
}

int lj_gc2_markobj(global_State *g, GCobj *o)
{
  void *base;
  int marked;
  int traversable;
  if (!o)
    return 0;
  base = gc2_mark_base(o);
  marked = lj_gc2_markmem(g, base);
  traversable = gc2_mark_base_traversable(g, base);
  if (marked && (g->gc2.phase == LJ_GC2_MARK || g->gc2.phase == LJ_GC2_WEAK) &&
      (traversable || o->gch.gct == ~LJ_TUDATA)) {
    if (traversable) {
      int pushed = lj_gc2_ssb_push(g, o);  /* 05 section 5.6.1. */
      lj_assertG(pushed, "gc2 SSB push failed for marked traversable object");
      UNUSED(pushed);
    } else {
      gc2_traverse_udata(g, gco2ud(o));
    }
  }
  return marked;
}

static int gc2_markobj_worker(global_State *g, GCobj *o)
{
  void *base;
  int marked;
  int traversable;
  if (!o)
    return 0;
  base = gc2_mark_base(o);
  marked = lj_gc2_markmem(g, base);
  traversable = gc2_mark_base_traversable(g, base);
  if (marked && (traversable || o->gch.gct == ~LJ_TUDATA)) {
    if (traversable) {
      int pushed = gc2_grey_push(g, o);  /* 05 section 5.6.3. */
      lj_assertG(pushed, "gc2 worker grey push failed for marked object");
      UNUSED(pushed);
    } else {
      gc2_traverse_udata(g, gco2ud(o));
    }
  }
  return marked;
}

static void gc2_mark_tv_worker(global_State *g, cTValue *tv)
{
  lj_assertG(!tvisgcv(tv) || (~itype(tv) == gcval(tv)->gch.gct),
	     "TValue and GC type mismatch");
  if (tvisgcv(tv))
    gc2_markobj_worker(g, gcV(tv));
}

static void gc2_note_weak_table(global_State *g, GCtab *t, int weak)
{
  if (!weak)
    return;
  if (gc2_tab_is_ffi_fin(g, t))
    return;  /* FFI finalizer registry is owned by FINREG, not weak clear. */
  la_add64_rlx(&g->gc2.weak_tables_seen, 1);
  if (weak & LJ_GC_WEAKKEY)
    la_add64_rlx(&g->gc2.weak_tables_weakkey, 1);
  if (weak & LJ_GC_WEAKVAL)
    la_add64_rlx(&g->gc2.weak_tables_weakval, 1);
  if (weak == LJ_GC_WEAK)
    la_add64_rlx(&g->gc2.weak_tables_allweak, 1);
  gc2_weak_record(g, t);
}

#if LJ_HASJIT
static void gc2_marktrace_worker(global_State *g, TraceNo traceno)
{
  if (traceno) {
    GCtrace *T = traceref(G2J(g), traceno);
    if (T)
      gc2_markobj_worker(g, obj2gco(T));
  }
}
#endif

static int gc2_traverse_tab(global_State *g, GCtab *t)
{
  GCtab *mt = tabref_acq(t->metatable);
  int weak = gc2_tab_weak_mode(g, t, mt);
  gc2_note_weak_table(g, t, weak);  /* 05 section 5.8 discovery scaffold. */
  if (t->acap > 0)
    lj_gc2_markmem(g, lj_tab_array_acq(t));
  {
    Node *node = lj_tab_node_acq(t);
    if (lj_tab_node_hmask_acq(node) > 0)
      lj_gc2_markmem(g, lj_tab_node_hdrw(node));
  }
  if (mt)
    gc2_markobj_worker(g, obj2gco(mt));
  if (weak == LJ_GC_WEAK)
    return weak;
  if (!(weak & LJ_GC_WEAKVAL)) {
    MSize i, asize = lj_tab_asize_acq(t);
    TValue *array = lj_tab_array_acq(t);
    for (i = 0; i < asize; i++) {
      TValue val;
      lj_tv_load_acq(&val, &array[i]);
      gc2_mark_tv_worker(g, &val);
    }
  }
  {
    Node *node = lj_tab_node_acq(t);
    MSize i, hmask = lj_tab_node_hmask_acq(node);
    if (hmask > 0) {
      for (i = 0; i <= hmask; i++) {
	Node *n = &node[i];
	TValue key, val;
	lj_tv_load_acq(&val, &n->val);
	if (!tvisnil(&val)) {
	  lj_tv_load_acq(&key, &n->key);
	  lj_assertG(!tvisnil(&key), "mark of nil key in non-empty slot");
	  if (!(weak & LJ_GC_WEAKKEY)) gc2_mark_tv_worker(g, &key);
	  if (!(weak & LJ_GC_WEAKVAL)) gc2_mark_tv_worker(g, &val);
	}
      }
    }
  }
  return weak;
}

static void gc2_traverse_udata(global_State *g, GCudata *ud)
{
  GCtab *mt = tabref_acq(ud->metatable);
  GCtab *env = tabref_acq(ud->env);
  uint8_t udtype = lj_udata_udtype_acq(ud);
  if (mt)
    gc2_markobj_worker(g, obj2gco(mt));
  if (env)
    gc2_markobj_worker(g, obj2gco(env));
  if (LJ_HASBUFFER && udtype == UDTYPE_BUFFER) {
    SBufExt *sbx = (SBufExt *)uddata(ud);
    GCobj *ref;
    if (!sbufiscoworborrow(sbx))
      lj_gc2_markmem(g, sbx->b);
    ref = gcref_acq(sbx->cowref);
    if (sbufiscow(sbx) && ref)
      gc2_markobj_worker(g, ref);
    ref = gcref_acq(sbx->dict_str);
    if (ref)
      gc2_markobj_worker(g, ref);
    ref = gcref_acq(sbx->dict_mt);
    if (ref)
      gc2_markobj_worker(g, ref);
  }
  if (udtype == UDTYPE_CHANNEL) {
    LJChan *ch = (LJChan *)uddata(ud);
    uint32_t i;
    for (i = 0; i < ch->cap; i++) {
      TValue tv;
      lj_tv_load_acq(&tv, &ch->slot[i].tv);
      gc2_mark_tv_worker(g, &tv);  /* 09 section 9.5. */
    }
  }
  if (udtype == UDTYPE_THREAD) {
    LJThread *th = (LJThread *)uddata(ud);
    if (th->L)
      gc2_markobj_worker(g, obj2gco(th->L));  /* 09 section 9.2. */
  }
}

static void gc2_traverse_upval(global_State *g, GCupval *uv)
{
  TValue tv;
  lj_tv_load_acq(&tv, uvval(uv));
  gc2_mark_tv_worker(g, &tv);
}

static void gc2_traverse_func(global_State *g, GCfunc *fn)
{
  GCtab *env = tabref_acq(fn->c.env);
  if (env)
    gc2_markobj_worker(g, obj2gco(env));
  if (isluafunc(fn)) {
    uint32_t i;
    lj_assertG(fn->l.nupvalues <= funcproto(fn)->sizeuv,
	       "function upvalues out of range");
    gc2_markobj_worker(g, obj2gco(funcproto(fn)));
    for (i = 0; i < fn->l.nupvalues; i++)
      gc2_markobj_worker(g, obj2gco(&gcref(fn->l.uvptr[i])->uv));
  } else {
    uint32_t i;
    for (i = 0; i < fn->c.nupvalues; i++) {
      TValue tv;
      lj_tv_load_acq(&tv, &fn->c.upvalue[i]);
      gc2_mark_tv_worker(g, &tv);
    }
  }
}

#if LJ_HASJIT
static void gc2_traverse_trace(global_State *g, GCtrace *T)
{
  IRRef ref;
  if (T->traceno == 0)
    return;
  for (ref = T->nk; ref < REF_TRUE; ref++) {
    IRIns *ir = &T->ir[ref];
    if (ir->o == IR_KGC)
      gc2_markobj_worker(g, ir_kgc(ir));
    if (irt_is64(ir->t) && ir->o != IR_KNULL)
      ref++;
  }
  gc2_marktrace_worker(g, trace_link_acq(T));
  gc2_marktrace_worker(g, trace_nextroot_acq(T));
  gc2_marktrace_worker(g, trace_nextside_acq(T));
  gc2_markobj_worker(g, gcref(T->startpt));
}
#endif

static void gc2_traverse_proto(global_State *g, GCproto *pt)
{
  ptrdiff_t i;
  gc2_markobj_worker(g, obj2gco(proto_chunkname(pt)));
  for (i = -(ptrdiff_t)pt->sizekgc; i < 0; i++)
    gc2_markobj_worker(g, proto_kgc(pt, i));
#if LJ_HASJIT
  gc2_marktrace_worker(g, proto_trace_acq(pt));
#endif
}

static TValue *gc2_stack_scan_top_worker(global_State *g, lua_State *L)
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
      gc2_markobj_worker(g, obj2gco(fn));
  }
  top++;
  if (top > tvref(L->maxstack))
    top = tvref(L->maxstack);
  return top;
}

static void gc2_traverse_thread(global_State *g, lua_State *th)
{
  GCobj *mt, *uv;
  TValue *o, *top;
  TValue tv;
  if (!th || tvref(th->stack) == NULL)
    return;
  lj_gc2_markmem(g, tvref(th->stack));
  top = gc2_stack_scan_top_worker(g, th);
  for (o = tvref(th->stack) + 1 + LJ_FR2; o < top; o++) {
    lj_tv_load_acq(&tv, o);
    gc2_mark_tv_worker(g, &tv);
  }
  {
    GCtab *env = tabref_acq(th->env);
    if (env)
      gc2_markobj_worker(g, obj2gco(env));
  }
  mt = gcref_acq(th->mt_thread);
  if (mt != NULL)
    gc2_markobj_worker(g, mt);
  for (uv = gcref(th->openupval); uv != NULL; uv = gcnext(uv)) {
    gc2_markobj_worker(g, uv);
    if (uv->gch.gct == ~LJ_TUPVAL) {
      TValue tv;
      lj_tv_load_acq(&tv, uvval(gco2uv(uv)));
      gc2_mark_tv_worker(g, &tv);
    }
  }
}

static void gc2_traverse_obj(global_State *g, GCobj *o)
{
  int gct = o->gch.gct;
  if (LJ_LIKELY(gct == ~LJ_TTAB)) {
    (void)gc2_traverse_tab(g, gco2tab(o));
  } else if (LJ_LIKELY(gct == ~LJ_TFUNC)) {
    gc2_traverse_func(g, gco2func(o));
  } else if (LJ_LIKELY(gct == ~LJ_TPROTO)) {
    gc2_traverse_proto(g, gco2pt(o));
  } else if (LJ_LIKELY(gct == ~LJ_TTHREAD)) {
    gc2_traverse_thread(g, gco2th(o));
  } else if (gct == ~LJ_TUPVAL) {
    gc2_traverse_upval(g, gco2uv(o));
  } else if (gct == ~LJ_TUDATA) {
    gc2_traverse_udata(g, gco2ud(o));
#if LJ_HASJIT
  } else if (gct == ~LJ_TTRACE) {
    gc2_traverse_trace(g, gco2trace(o));
#endif
  } else {
    lj_assertG(gct == ~LJ_TSTR || gct == ~LJ_TCDATA,
	       "bad GC type %d", gct);
  }
}

static uint32_t gc2_drain_grey(global_State *g, uint32_t limit)
{
  uint32_t n = 0;
  while (g && n < limit && !gc2_grey_empty(g)) {
    GCobj *o = gc2_grey_pop(g);
    if (o) {
      gc2_traverse_obj(g, o);
      n++;
    }
  }
  if (n)
    la_add64_rlx(&g->gc2.grey_drained, n);
  return n;
}

static uint32_t gc2_worker_drain_inner(global_State *g, uint32_t limit,
				       uint32_t *progress)
{
  uint32_t phase, n = 0, converted = 0;
  if (progress)
    *progress = 0;
  if (!g || limit == 0)
    return 0;
  phase = la_load32_acq(&g->gc2.phase);
  if (phase != LJ_GC2_MARK && phase != LJ_GC2_WEAK)
    return 0;
  while (n < limit) {
    GCobj *o = lj_gc2_grey_steal(g);
    if (o) {
      gc2_traverse_obj(g, o);  /* 05 section 5.6.3 worker steal+trace. */
      n++;
      continue;
    }
    if (converted >= limit)
      break;
    {
      uint32_t moved = gc2_drain_published_ssb_to_grey(g, limit - converted);
      if (!moved)
	break;
      converted += moved;
    }
  }
  if (n || converted)
    la_add64_rlx(&g->gc2.worker_runs, 1);
  if (n) {
    la_add64_rlx(&g->gc2.grey_drained, n);
    la_add64_rlx(&g->gc2.worker_grey_drained, n);
  }
  if (converted)
    la_add64_rlx(&g->gc2.worker_ssb_converted, converted);
  if (progress) {
    if (converted > ~(uint32_t)0 - n)
      *progress = ~(uint32_t)0;
    else
      *progress = n + converted;
  }
  return n;
}

uint32_t lj_gc2_worker_drain(global_State *g, uint32_t limit)
{
  return gc2_worker_drain_inner(g, limit, NULL);
}

uint32_t lj_gc2_worker_drain_progress(global_State *g, uint32_t limit)
{
  uint32_t progress;
  (void)gc2_worker_drain_inner(g, limit, &progress);
  return progress;
}

static uint32_t gc2_worker_drain_budget(global_State *g, uint32_t limit)
{
  uint32_t n = 0;
  while (n < limit && !lj_gc2_ssb_empty(g)) {
    uint32_t step = lj_gc2_worker_drain_progress(g, limit - n);
    if (step == 0)
      break;
    if (step > limit - n)
      n = limit;
    else
      n += step;
  }
  return n;
}

uint32_t lj_gc2_fixpoint_round(global_State *g, lua_State *L, uint32_t limit)
{
  uint32_t phase, acked, fixpoint;
  if (!g || limit == 0)
    return 0;
  phase = la_load32_acq(&g->gc2.phase);
  if (phase != LJ_GC2_MARK)
    return 0;
  (void)la_xchg64_acqrel(&g->gc2.marks_this_round, 0);
  (void)gc2_worker_drain_budget(g, limit);  /* 05 section 5.7.1 pre-round drain. */
  acked = lj_gc2_handshake(g, LJ_GC2_HS_SCAN_ROOTS|LJ_GC2_HS_FLUSH_SSB);
  if (acked == 0 && L) {
    lj_gc2_scan_roots(g, L);
    (void)lj_gc2_flush_ssb(g, L2TG(L));
  }
  (void)gc2_worker_drain_budget(g, limit);  /* 05 section 5.7.1 post-root drain. */
  fixpoint = la_load64_acq(&g->gc2.marks_this_round) == 0 &&
	     lj_gc2_ssb_empty(g);
  la_add64_rlx(&g->gc2.fixpoint_rounds, 1);
  if (fixpoint)
    la_add64_rlx(&g->gc2.fixpoint_hits, 1);
  return fixpoint;
}

uint32_t lj_gc2_fixpoint_run(global_State *g, lua_State *L,
			     uint32_t max_rounds, uint32_t limit)
{
  uint32_t i;
  if (!g || max_rounds == 0 || limit == 0)
    return 0;
  for (i = 0; i < max_rounds; i++)
    if (lj_gc2_fixpoint_round(g, L, limit))
      return 1;
  return 0;
}

int lj_gc2_ismarkedmem(global_State *g, void *p)
{
  TGState *tg = gc2_tg_for_mem(g, p);
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
