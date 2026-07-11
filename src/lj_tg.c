/*
** Per-OS-thread state block scaffolding for LuaJIT-MT.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_tg_c
#define LUA_CORE

#include <stdlib.h>

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_buf.h"
#include "lj_dispatch.h"
#include "lj_err.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_profile.h"
#include "lj_prng.h"
#include "lj_safepoint.h"
#include "lj_str.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_vm.h"

static void tg_root_anchor_block_init(TGRootAnchorBlock *block)
{
  uint32_t i;
  lj_tg_root_anchor_next_rel(block, NULL);
  for (i = 0; i < TG_ROOT_ANCHOR_SLOTS; i++)
    setnilV(&block->slot[i]);
}

static TValue *tg_root_anchor_slot_create(lua_State *L, TGState *tg,
					  uint32_t idx)
{
  TGRootAnchorBlock *block = &tg->root_anchor;
  uint32_t blockidx = idx / TG_ROOT_ANCHOR_SLOTS;
  while (blockidx-- != 0) {
    TGRootAnchorBlock *next = lj_tg_root_anchor_next_acq(block);
    if (!next) {
      next = lj_mem_newt(L, sizeof(TGRootAnchorBlock), TGRootAnchorBlock);
      tg_root_anchor_block_init(next);
      lj_tg_root_anchor_next_rel(block, next);
    }
    block = next;
  }
  return &block->slot[idx % TG_ROOT_ANCHOR_SLOTS];
}

TValue *lj_tg_root_anchor_slot_acq(TGState *tg, uint32_t idx)
{
  TGRootAnchorBlock *block;
  uint32_t blockidx;
  if (!tg)
    return NULL;
  block = &tg->root_anchor;
  blockidx = idx / TG_ROOT_ANCHOR_SLOTS;
  while (blockidx-- != 0) {
    block = lj_tg_root_anchor_next_acq(block);
    if (!block)
      return NULL;
  }
  return &block->slot[idx % TG_ROOT_ANCHOR_SLOTS];
}

TValue *lj_tg_root_anchor_push(lua_State *L, TGState *tg, cTValue *tv,
			       uint32_t *idxp)
{
  uint32_t idx;
  TValue *slot;
  if (!tg)
    return NULL;
  idx = lj_tg_root_anchor_top_acq(tg);
  if (LJ_UNLIKELY(idx == ~(uint32_t)0))
    lj_err_mem(L);
  slot = tg_root_anchor_slot_create(L, tg, idx);
  copyTVrel(L, slot, tv);
  lj_tg_root_anchor_top_rel(tg, idx + 1);
  if (idxp)
    *idxp = idx;
  return slot;
}

void lj_tg_root_anchor_pop(TGState *tg, uint32_t idx)
{
  TValue *slot = lj_tg_root_anchor_slot_acq(tg, idx);
  uint32_t top;
  if (!slot)
    return;
  setnilV(slot);
  top = lj_tg_root_anchor_top_acq(tg);
  if (top == idx + 1)
    lj_tg_root_anchor_top_rel(tg, idx);
}

static void tg_root_anchor_fini(global_State *g, TGState *tg)
{
  TGRootAnchorBlock *block, *next;
  if (!tg)
    return;
  block = lj_tg_root_anchor_next_acq(&tg->root_anchor);
  lj_tg_root_anchor_next_rel(&tg->root_anchor, NULL);
  while (block) {
    next = lj_tg_root_anchor_next_acq(block);
    lj_mem_freet(g, block);
    block = next;
  }
  lj_tg_root_anchor_top_rel(tg, 0);
}

static void tg_init_ssb(TGState *tg)
{
  la_store32_rlx(&tg->ssb_refs, 0);
  tg->ssb_node[0].pad = 0;
  lj_gc2_ssb_owner_rel(&tg->ssb_node[0], tg);
  lj_gc2_ssb_next_rel(&tg->ssb_node[0], NULL);
  lj_gc2_ssb_count_rel(&tg->ssb_node[0], 0);
  tg->ssb_node[1].pad = 0;
  lj_gc2_ssb_owner_rel(&tg->ssb_node[1], tg);
  lj_gc2_ssb_next_rel(&tg->ssb_node[1], NULL);
  lj_gc2_ssb_count_rel(&tg->ssb_node[1], 0);
  lj_tg_ssb_active_rel(tg, &tg->ssb_node[0]);
  lj_tg_ssb_free_store_rlx(tg, &tg->ssb_node[1]);
  lj_tg_ssb_base_rel(tg, tg->ssb_node[0].slot);
  lj_tg_ssb_next_rel(tg, tg->ssb_node[0].slot);
  lj_tg_ssb_end_rel(tg, tg->ssb_node[0].slot + TG_GC2_SSB_SLOTS);
}

void lj_tg_fini_ssb(TGState *tg)
{
  GC2SSBNode *node, *next;
  if (!tg)
    return;
  lj_assertX(lj_tg_ssb_refs_acq(tg) == 0,
	     "finalizing TG with published SSB nodes");
  node = lj_tg_ssb_active_acq(tg);
  if (node && (node->pad & TG_GC2_SSB_DYNAMIC))
    free(node);
  lj_tg_ssb_active_rel(tg, NULL);
  lj_tg_ssb_base_rel(tg, NULL);
  lj_tg_ssb_next_rel(tg, NULL);
  lj_tg_ssb_end_rel(tg, NULL);
  node = lj_tg_ssb_free_acq(tg);
  lj_tg_ssb_free_store_rlx(tg, NULL);
  while (node) {
    next = lj_gc2_ssb_next_acq(node);
    if (node->pad & TG_GC2_SSB_DYNAMIC)
      free(node);
    node = next;
  }
}

static void tg_init_common(global_State *g, TGState *tg, lua_State *L)
{
  tg->gl = g;
  lj_tg_store_cur_L(tg, L);
  lj_tg_store_thread_L(tg, L);
  tg->ffi_xsave_root = NULL;
  tg->ffi_xsave_baseslot = 0;
  tg->ffi_xsave_nslots = 0;
  tg->vmstate = ~LJ_VMST_INTERP;
  tg->profile_vmstate = 'N';
  tg->prng = g->prng;
  tg->strtab_active_hdr = NULL;
  tg->strtab_active_depth = 0;
  tg->strtab_active_epoch = 0;
  tg->strq_active_hdr = NULL;
  tg->strq_active_depth = 0;
  tg->strq_active_epoch = 0;
  tg->strid_next = 0;
  tg->strid_end = 0;
  tg->strnum_credit = 0;
  lj_tg_fini_state_store_rlx(tg, TG_FINI_LIVE);
  lj_tg_worker_retire_next_rel(tg, NULL);
  setnilV(&tg->tmptv);
  setnilV(&tg->tmptv2);
  tg_root_anchor_block_init(&tg->root_anchor);
  lj_tg_root_anchor_top_rel(tg, 0);
  lj_tg_gcroot_pending_store_rlx(tg, NULL);
  lj_tg_gcroot_pending_after_main_store_rlx(tg, NULL);
  tg_init_ssb(tg);
  lj_buf_init(NULL, &tg->tmpbuf);
#if LJ_HASJIT
  memcpy(tg->hotcount, G2GG(g)->hotcount, sizeof(tg->hotcount));
#endif
  memcpy(tg->dispatch, G2GG(g)->dispatch, sizeof(tg->dispatch));
}

void lj_tg_init(GG_State *GG, int alloc_ready)
{
  TGState *tg = &GG->main_tg;
  global_State *g = &GG->g;
  lua_State *L = &GG->L;
  uint32_t tid = lj_thr_newid();
  g->main_tg = tg;
  lj_tg_tid_rel(tg, tid);
  L->tg_hint = tg;
  lj_state_owner_rel(L, tid);
  if (!lj_thr_get_tg())
    lj_thr_set_tg(tg);  /* 03 section 3.2: bootstrap main OS-thread TLS. */
  if (!alloc_ready)
    lj_arena_alloc_init(&tg->alloc);
  else {
    lj_tg_flags_or_rlx(tg, TGF_ARENA_INTERNAL);
  }
  lj_arena_allocd_init(&tg->allocd, &tg->alloc, &tg->prng, 0);
  lj_arena_alloc_owner_tg_rel(&tg->alloc, tg);
  if (alloc_ready && lj_arena_hugetab_init(&tg->huge, TG_HUGETAB_BITS)) {
    lj_tg_flags_or_rlx(tg, TGF_HUGETAB);
    lj_arena_allocd_sethugetab(&tg->allocd, &tg->huge);
  }
  tg_init_common(g, tg, L);
}

void lj_tg_fini(global_State *g)
{
  if (g->main_tg) {
    lj_str_flush_num_credit(g, g->main_tg);
    tg_root_anchor_fini(g, g->main_tg);
    lj_tg_fini_ssb(g->main_tg);
    lj_buf_free(g, &g->main_tg->tmpbuf);
    if (lj_tg_flags_test_acq(g->main_tg, TGF_HUGETAB))
      lj_arena_hugetab_fini(&g->main_tg->huge);
    lj_arena_alloc_fini(&g->main_tg->alloc);
  }
}

void lj_tg_init_thread(global_State *g, TGState *tg, lua_State *L,
		       int arena_internal)
{
  memset(tg, 0, sizeof(*tg));
  if (L) {
    L->tg_hint = tg;
    setmref(L->glref, g);
  }
  lj_arena_alloc_init(&tg->alloc);
  if (arena_internal) {
    lj_tg_flags_or_rlx(tg, TGF_ARENA_INTERNAL);
    lj_arena_alloc_set_registry(&tg->alloc,
      (HugeTab *)gc2_small_arena_tab_acq(g));
  }
  lj_arena_allocd_init(&tg->allocd, &tg->alloc, &tg->prng, 0);
  lj_arena_alloc_owner_tg_rel(&tg->alloc, tg);
  if (arena_internal && lj_arena_hugetab_init(&tg->huge, TG_HUGETAB_BITS)) {
    lj_tg_flags_or_rlx(tg, TGF_HUGETAB);
    lj_arena_allocd_sethugetab(&tg->allocd, &tg->huge);
  }
  tg_init_common(g, tg, L);
}

void lj_tg_derive_prng(global_State *g, TGState *tg, uint32_t tid)
{
  TGState *parent = lj_thr_get_tg();
  const PRNGState *parent_prng =
    parent && parent != tg && parent->gl == g ? &parent->prng : &g->prng;
  if (tid != 0)
    lj_prng_derive(&tg->prng, parent_prng, tid);
}

static int tg_fini_thread(global_State *g, TGState *tg, int terminal)
{
  uint8_t expect = TG_FINI_LIVE;
  if (!tg)
    return 1;
  /* LIVE->BUSY is the physical-finalization LP. Valid runtime reclaimers and
  ** the worker-retire owner are serialized, so observing BUSY is an ownership
  ** violation, never a reason to wait. DONE release-publishes every pointer
  ** clear and allocator unmap; a later worker-retire storage owner may then
  ** call this routine idempotently before free(tg). */
  if (!lj_tg_fini_state_cas(tg, &expect, TG_FINI_BUSY)) {
    if (expect == TG_FINI_DONE)
      return 1;
    /* Storage owners commonly finalize immediately before free(tg). BUSY can
    ** therefore never degrade to a failed try result: doing so would let an
    ** older void-style caller release storage under the active finalizer. */
    abort();
  }
  lj_str_flush_num_credit(g, tg);
  tg_root_anchor_fini(g, tg);
  lj_tg_fini_ssb(tg);
  lj_buf_free(g, &tg->tmpbuf);
  lj_buf_init(NULL, &tg->tmpbuf);
  if (lj_tg_flags_test_acq(tg, TGF_HUGETAB)) {
    if (terminal)
      (void)lj_arena_hugetab_fini_all(&tg->huge);
    else
      lj_arena_hugetab_fini(&tg->huge);
    lj_tg_flags_and_rlx(tg, (uint8_t)~TGF_HUGETAB);
    lj_arena_allocd_sethugetab(&tg->allocd, NULL);
  }
  lj_arena_alloc_fini(&tg->alloc);
  lj_tg_fini_state_rel(tg, TG_FINI_DONE);
  return 1;
}

int lj_tg_fini_thread(global_State *g, TGState *tg)
{
  return tg_fini_thread(g, tg, 0);
}

static void tg_adopt_gc2_phase(global_State *g, TGState *tg)
{
  uint32_t phase = gc2_phase_acq(g);
  if (phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK) {
    lj_tg_mark_active_rel(tg, 1);
    lj_tg_alloc_black_rel(tg, 1);
  } else if (phase == LJ_GC2_SWEEP) {
    lj_tg_mark_active_rel(tg, 0);
    lj_tg_alloc_black_rel(tg, (uint8_t)(gc2_cycle_sweep_minor_acq(g) == 0));
  } else {
    lj_tg_mark_active_rel(tg, gc2_generational_acq(g) != 0);
    lj_tg_alloc_black_rel(tg, 0);
  }
}

static int tg_attach_trace_boundary_live(global_State *g)
{
  return gc2_hs_leader_acq(g) != 0 &&
	 (gc2_hs_actions_acq(g) &
	  (LJ_GC2_HS_EXIT_TRACES|LJ_GC2_HS_FLUSHJ)) != 0;
}

static void tg_attach_wait_trace_boundary(global_State *g, TGState *tg)
{
  uint32_t leader;
  lua_State *L = lj_tg_load_thread_L(tg);
  while (tg_attach_trace_boundary_live(g) &&
	 (leader = gc2_hs_leader_acq(g)) != 0) {
    if (L && (lj_tg_reqmask_acq(tg) != 0 || lj_tg_poll_acq(tg) != 0)) {
      (void)lj_safepoint_ack(L);
      continue;
    }
    gc2_hs_leader_futex_wait(g, leader, 1000000);
  }
}

static void tg_attach_catchup(global_State *g, TGState *tg)
{
  uint64_t epoch = gc2_hs_epoch_acq(g);
  uint32_t pending = gc2_hs_pending_acq(g);
  uint32_t actions = pending ? gc2_hs_actions_acq(g) : 0;
  lj_tg_hs_epoch_ack_store_rlx(tg, epoch);
  if (actions) {
    lj_safepoint_apply_tg(g, tg, actions);
    lj_tg_reqmask_rel(tg, 0);
    lj_tg_poll_rel(tg, 0);
    lj_tg_hs_epoch_ack_rel(tg, epoch);  /* 09 section 9.3 self-ack. */
    if (actions & (LJ_GC2_HS_EXIT_TRACES|LJ_GC2_HS_FLUSHJ))
      tg_attach_wait_trace_boundary(g, tg);
  }
}

void lj_tg_attach(global_State *g, TGState *tg)
{
  TGState *head;
  if (!g || !tg)
    return;
  lj_tg_poll_store_rlx(tg, 0);
  lj_tg_reqmask_store_rlx(tg, 0);
  tg_adopt_gc2_phase(g, tg);  /* 09 section 9.3 attach catch-up scaffold. */
  tg_attach_catchup(g, tg);
  lj_tg_flags_and_rlx(tg, (uint8_t)~TGF_DEAD);
  do {
    TGState *cur;
    head = gc2_tg_list_acq(g);  /* 05 section 5.4.1. */
    for (cur = head; cur != NULL;) {
      TGState *next = lj_tg_next_acq(cur);
      if (cur == tg) {
	if (next == tg)
	  lj_tg_next_rel(tg, NULL);
	return;
      }
      if (next == cur)
	break;
      cur = next;
    }
    lj_tg_next_rel(tg, head);
  } while (!gc2_tg_list_cas(g, &head, tg));  /* 05 section 5.4.1 CAS-prepend. */
  gc2_n_threads_add_rlx(g, 1);  /* Live TG count; list keeps dead nodes. */
  tg_attach_wait_trace_boundary(g, tg);
  (void)lj_gc_flush_root_pending(g);
}

void lj_tg_detach(global_State *g, TGState *tg)
{
  uint8_t oldflags;
  lua_State *thread_L;
  if (!g || !tg)
    return;
  thread_L = lj_tg_load_thread_L(tg);
  if (thread_L &&
      (lj_tg_reqmask_acq(tg) != 0 || lj_tg_poll_acq(tg) != 0))
    (void)lj_safepoint_ack(thread_L);  /* Leaving TG owns its ack. */
  (void)lj_gc_flush_root_pending(g);
  (void)lj_gc2_flush_ssb_detach(g, tg);  /* Terminal, allocation-free flush. */
  (void)lj_gc2_flush_alloc(g, tg);  /* 04 section 4.8 detach accounting. */
  lj_str_flush_num_credit(g, tg);
  /* tmpbuf is owner-private transient storage. Detach is its final owner
  ** boundary, so release it while this TG is still a live registry lookup;
  ** global root scans no longer need a dead-storage exception and later
  ** allocator transfer/finalization sees an idempotently empty buffer. */
  lj_buf_free(g, &tg->tmpbuf);
  lj_buf_init(NULL, &tg->tmpbuf);
  /* Clear every remotely sampled owner publication before DEAD becomes
  ** visible. Subsequent foreign state-release cleanup remains protected by
  ** mt_live, which prevents physical registry reclamation until that lookup
  ** and every other VM access is complete. */
  lj_tg_store_cur_L(tg, NULL);
  lj_tg_store_thread_L(tg, NULL);
  lj_tg_store_thread_ud(tg, NULL);
  lj_tg_in_native_store_rlx(tg, 0);
#if LJ_HASJIT
  lj_tg_store_jit_base(tg, NULL);
#endif
#if LJ_HASFFI
  lj_tg_ffi_call_func_rel(tg, NULL);
#endif
  /* POSIX profiling samples the current TLS TG from a signal handler. Since
  ** delivery is same-thread, clearing TLS before the registry retirement LP
  ** guarantees that a handler either finishes against the still-live TG or
  ** observes NULL; it can never retain a post-decrement raw pointer. */
  if (lj_thr_get_tg() == tg)
    lj_thr_set_tg(NULL);
  la_fence_rel();
  oldflags = lj_tg_flags_or_rlx(tg, TGF_DEAD);  /* 05 section 5.4.1. */
  (void)lj_safepoint_retire_dead_tg(g, tg);
  if (!(oldflags & TGF_DEAD))
    (void)gc2_n_threads_sub_acqrel(g, 1);
}

static int tg_transfer_dead_alloc(global_State *g, TGState *tg)
{
  TGState *main_tg = g ? g->main_tg : NULL;
  if (!lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL))
    return 1;
  if (!main_tg || !lj_tg_flags_test_acq(main_tg, TGF_ARENA_INTERNAL))
    return 0;
  if (lj_tg_flags_test_acq(tg, TGF_HUGETAB)) {
    if (!lj_tg_flags_test_acq(main_tg, TGF_HUGETAB) ||
	!lj_arena_hugetab_transfer(&main_tg->huge, &tg->huge,
				   lj_arena_alloc_owner_acq(&main_tg->alloc)))
      return 0;
    lj_arena_hugetab_fini(&tg->huge);
    lj_tg_flags_and_rlx(tg, (uint8_t)~TGF_HUGETAB);
    lj_arena_allocd_sethugetab(&tg->allocd, NULL);
  }
  (void)lj_arena_alloc_transfer(&main_tg->alloc, &tg->alloc);
  lj_arena_allocd_init(&tg->allocd, &tg->alloc, &tg->prng, 0);
  lj_tg_flags_and_rlx(tg, (uint8_t)~TGF_ARENA_INTERNAL);
  return 1;
}

static int tg_reclaim_dead_admissible(global_State *g, int terminal)
{
  TGState *self;
  if (!g || gc2_n_threads_acq(g) != 1 ||
      gc2_hs_pending_acq(g) != 0 || gc2_hs_leader_acq(g) != 0 ||
      mt_live_acq(g) != 0 ||
      mt_entering_acq(g) != 0 || gc2_n_workers_acq(g) != 0 ||
      gc2_worker_active_acq(g) != 0)
    return 0;
  if (terminal)
    return mt_shutdown_acq(g) != 0;
  self = lj_thr_get_tg();
  return self == g->main_tg;
}

static int tg_reclaim_writer_try(global_State *g, int terminal)
{
  uint32_t expect = 0;
  if (!tg_reclaim_dead_admissible(g, terminal) ||
      !gc2_smr_reclaiming_cas(g, &expect, 1))
    return 0;
  /* Publish the shared metadata-reclaim gate before testing readers. A reader
  ** already inside keeps its counted lease and makes this pass abandon; a new
  ** reader observes the gate and backs out. GC workers use the inverse
  ** worker_active/reclaiming protocol, so this writer never waits for either. */
  if (gc2_smr_readers_acq(g) != 0 ||
      !tg_reclaim_dead_admissible(g, terminal)) {
    gc2_smr_reclaiming_rel(g, 0);
    return 0;
  }
  expect = 0;
  if (!gc2_tg_reclaiming_cas(g, &expect, 1)) {
    gc2_smr_reclaiming_rel(g, 0);
    return 0;
  }
  /* The TG gate excludes attach/list writers; the second full recheck closes
  ** both admission races before any next_tg link or TG body can be removed. */
  if (gc2_smr_readers_acq(g) != 0 ||
      !tg_reclaim_dead_admissible(g, terminal)) {
    gc2_tg_reclaiming_rel(g, 0);
    gc2_smr_reclaiming_rel(g, 0);
    return 0;
  }
  return 1;
}

static void tg_reclaim_writer_leave(global_State *g)
{
  gc2_tg_reclaiming_rel(g, 0);
  gc2_smr_reclaiming_rel(g, 0);
}

static int tg_terminal_pending_roots_empty(global_State *g)
{
  TGState *tg;
  if (lj_gcroot_pending_hint_acq(g) != 0)
    return 0;
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg))
    if (lj_tg_gcroot_pending_acq(tg) != NULL ||
	lj_tg_gcroot_pending_after_main_acq(tg) != NULL)
      return 0;
  return 1;
}

static int tg_lua_storage_owner_follows(TGState *tg, uint32_t owner_tid)
{
  TGState *cur;
  uint32_t n = 0;
  if (!tg || owner_tid == 0 || owner_tid == lj_tg_tid_acq(tg))
    return 0;
  for (cur = lj_tg_next_acq(tg); cur != NULL; cur = lj_tg_next_acq(cur)) {
    if (lj_tg_tid_acq(cur) == owner_tid)
      return 1;
    if (cur == lj_tg_next_acq(cur) || ++n >= 1000000u)
      return 0;
  }
  return 0;
}

static int tg_worker_retired_contains(global_State *g, TGState *target)
{
  TGState *tg;
  uint32_t n = 0;
  for (tg = (TGState *)gc2_worker_tg_retired_acq(g);
       tg != NULL; tg = lj_tg_worker_retire_next_acq(tg)) {
    if (tg == target)
      return 1;
    if (tg == lj_tg_worker_retire_next_acq(tg) || ++n >= 1000000u)
      return 0;
  }
  return 0;
}

static uint32_t tg_reclaim_dead(global_State *g, int terminal, int orphan)
{
  TGState *prev, *tg;
  uint32_t reclaimed = 0;
  /* Flush before publishing the SMR writer gate. Root validation may itself
  ** take a registry read lease; doing it as the writer would self-deny. The
  ** late orphan path runs after the Lua stack and all objects are gone, so it
  ** must never try to repair/flush roots: close_state has already proved and
  ** published empty pending stacks at its earlier post-freeall boundary. */
  if (!tg_reclaim_dead_admissible(g, terminal))
    return 0;
  if (!orphan)
    (void)lj_gc_flush_root_pending(g);
  if (!tg_reclaim_writer_try(g, terminal))
    return 0;
  if (orphan && !tg_terminal_pending_roots_empty(g)) {
    tg_reclaim_writer_leave(g);
    return 0;
  }
restart:
  prev = NULL;
  tg = gc2_tg_list_acq(g);
  while (tg != NULL) {
    TGState *next = lj_tg_next_acq(tg);
    if (lj_tg_flags_test_acq(tg, TGF_DEAD)) {
      uint8_t flags = lj_tg_flags_acq(tg);
      uint8_t heap_tg = (uint8_t)(flags & TGF_HEAP);
      uint8_t lua_tg = (uint8_t)(flags & TGF_LUA_ALLOC);
      uint8_t deferred_lua_tg = (uint8_t)
	((flags & (TGF_LUA_ALLOC|TGF_DEFER_FREE)) ==
	 (TGF_LUA_ALLOC|TGF_DEFER_FREE));
      if ((heap_tg && lua_tg) ||
	  ((flags & TGF_DEFER_FREE) && !lua_tg))
	abort();
      if (lj_tg_ssb_refs_acq(tg) != 0) {
	lj_assertG(!orphan, "published SSB pin survived terminal freeall");
	prev = tg;  /* Published embedded nodes still name this TG storage. */
	tg = next;
	continue;
      }
	if (orphan && lua_tg) {
	  uint32_t storage_owner;
	  /* threading.spawn allocates the TG from its already-linked parent,
	  ** then the child CAS-prepends on attach. A retained TG's userdata
	  ** finalizer publishes DEFER_FREE; runtime parent reclamation instead
	  ** rewrites the storage header owner to the still-last main TG. Thus the
	  ** Lua TG body owner must occur strictly later in this newest-first list. */
	  if (!deferred_lua_tg || g->allocf != lj_arena_allocf)
	    abort();
	  storage_owner = lj_arena_owner_acq(lj_arena_of(tg));
	  if (!tg_lua_storage_owner_follows(tg, storage_owner))
	    abort();
	}
	if (orphan && !heap_tg && !lua_tg &&
	    !tg_worker_retired_contains(g, tg))
	  abort();  /* Only the embedded retire list may own unflagged storage. */
	/* Runtime reclamation preserves every allocation by moving ownership to
	** the main TG. After all GC/runtime destructors have run, the terminal
	** orphan pass instead destroys the now-dead allocator in place; this path
	** never depends on destination hugetab capacity. */
      if (!orphan && !tg_transfer_dead_alloc(g, tg)) {
	prev = tg;  /* Keep owner lookup live until allocator transfer succeeds. */
	tg = next;
	continue;
      }
	if (orphan && !tg_fini_thread(g, tg, 1)) {
	  prev = tg;
	  tg = next;
	  continue;
	}
      if (prev) {
	lj_tg_next_rel(prev, next);
      } else {
	TGState *expect = tg;
	if (!gc2_tg_list_cas(g, &expect, next))
	  goto restart;
      }
      lj_tg_next_rel(tg, NULL);
      reclaimed++;
      if (heap_tg) {
	if (!orphan && !lj_tg_fini_thread(g, tg))
	  abort();
	free(tg);
	} else if (orphan && lua_tg) {
	/* freeall has destroyed the threading.thread userdata, so terminal
	** registry ownership supersedes its ordinary DEFER_FREE handoff. */
	lj_mem_freet(g, tg);
	} else if (deferred_lua_tg) {
	/* The threading.thread userdata relinquished this still-registered TG
	** while an embedded SSB publication pinned it. Its final reference has
	** now drained, so finish through the allocator which created the TG. */
	if (!lj_tg_fini_thread(g, tg))
	  abort();
	lj_mem_freet(g, tg);
      }
      tg = next;
      continue;
    }
    prev = tg;
    tg = next;
  }
  tg_reclaim_writer_leave(g);
  return reclaimed;
}

uint32_t lj_tg_reclaim_dead(global_State *g)
{
  return tg_reclaim_dead(g, 0, 0);
}

uint32_t lj_tg_reclaim_dead_terminal(global_State *g)
{
  /* A closing Lua universe may share its OS thread with another universe, so
  ** raw TLS legitimately need not name this GG's embedded main TG. Shutdown
  ** has already closed attach admission and joined every mutator/GC worker;
  ** the counter checks and writer CAS above are the terminal ownership proof.
  ** Keep the ordinary runtime path main-TLS-only. */
  return tg_reclaim_dead(g, 1, 0);
}

uint32_t lj_tg_reclaim_dead_terminal_orphans(global_State *g)
{
  /* This is valid only after freeall and every subsystem/raw destructor.
  ** Shutdown admission, the SMR/TG writer gates, and zero publishers prove
  ** that allocator mappings can be destroyed instead of transferred. */
  return tg_reclaim_dead(g, 1, 1);
}

TGState *lj_tg_find_owner(global_State *g, uint32_t owner_tid)
{
  TGState *tg;
  if (!g || owner_tid == 0)
    return NULL;
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg)) {
    if (lj_tg_tid_acq(tg) == owner_tid)
      return tg;
  }
  return g->main_tg && lj_tg_tid_acq(g->main_tg) == owner_tid ?
	 g->main_tg : NULL;
}

TGState *lj_tg_thread_active(global_State *g, lua_State *L)
{
  uint32_t owner;
  TGState *tg = NULL;
  /*
  ** Resolve the live TG that currently owns L before a collector or safepoint
  ** treats its stack as owner-current. Claimed GC scans and dead TGs are not
  ** current mutators, and the TG must still publish L as cur_L.
  */
  if (!g || !L)
    return NULL;
  owner = lj_state_owner_acq(L);
  if (owner != 0 && owner != LJ_THREAD_GCSCAN)
    tg = lj_tg_find_owner(g, owner);
  else if (L == lj_tg_cur_L(g))
    tg = G2TG(g);
  if (!tg || lj_tg_flags_test_acq(tg, TGF_DEAD) ||
      lj_tg_load_cur_L(tg) != L)
    return NULL;
  return tg;
}

int lj_tg_any_jit_active(global_State *g)
{
#if LJ_HASJIT
  TGState *tg;
  int saw_tg = 0;
  if (!g)
    return 0;
  /*
  ** GC phase completion is global, while lj_tg_jit_base(g) intentionally reads
  ** only the caller's TG. A different TG may be executing trace code with live
  ** values only in machine registers or trace spill slots; stack scanners cannot
  ** prove those roots until the trace reaches an exit/safepoint. Cold GC gates
  ** therefore scan the registered TG list and defer final mark/weak/finalizer
  ** transitions while any live TG has a published trace base or positive trace
  ** vmstate.
  */
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg)) {
    saw_tg = 1;
    if (!lj_tg_flags_test_acq(tg, TGF_DEAD) &&
	lj_tg_jit_active_acq(tg))
      return 1;
  }
  if (!saw_tg) {
    tg = g->main_tg;
    if (tg && !lj_tg_flags_test_acq(tg, TGF_DEAD)) {
      if (lj_tg_jit_active_acq(tg))
	return 1;
      saw_tg = 1;
    }
  }
  /*
  ** The global jit_base/vmstate fields are a bootstrap mirror for code that has
  ** no TG yet. Once a TG is registered, x64 trace entry/exit updates TG-local
  ** state directly and the mirror may retain an old non-NULL base.
  */
  return !saw_tg &&
	 (mref_acq(g->jit_base, TValue) != NULL || vmstate_load_acq(g) > 0);
#else
  UNUSED(g);
  return 0;
#endif
}

#if LJ_PROFILE_TGLOCAL
static void tg_profile_overlay(TGState *tg)
{
  if (lj_tg_hookmask_load(tg) & HOOK_PROFILE) {
    uint32_t i;
    for (i = 0; i < BC_FUNCF; i++)
      tg->dispatch[i] = lj_vm_profhook;
    for (i = BC_CNEW; i <= BC_CSET; i++)
      tg->dispatch[i] = lj_vm_profhook;
  }
}
#endif

void lj_tg_sync_dispatch_tg(global_State *g, TGState *tg)
{
  if (g && tg) {
    memcpy(tg->dispatch, G2GG(g)->dispatch, sizeof(tg->dispatch));
#if LJ_PROFILE_TGLOCAL
    tg_profile_overlay(tg);
#endif
  }
}

void lj_tg_sync_dispatch(global_State *g)
{
  lj_tg_sync_dispatch_tg(g, G2TG(g));
}
