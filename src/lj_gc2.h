/*
** Concurrent GC scaffold.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_GC2_H
#define _LJ_GC2_H

#include "lj_obj.h"

#ifndef LJ_GC2_PARANOIA
#define LJ_GC2_PARANOIA		0
#endif

enum {
  LJ_GC2_IDLE,
  LJ_GC2_MARK,
  LJ_GC2_WEAK,
  LJ_GC2_SWEEP
};

#define LJ_GC2_HS_ENABLE_BARRIER	0x00000001u
#define LJ_GC2_HS_DISABLE_BARRIER	0x00000002u
#define LJ_GC2_HS_ALLOC_BLACK		0x00000004u
#define LJ_GC2_HS_ALLOC_WHITE		0x00000008u
#define LJ_GC2_HS_SCAN_ROOTS		0x00000010u
#define LJ_GC2_HS_FLUSH_SSB		0x00000020u
#define LJ_GC2_HS_RESET_ALLOC		0x00000040u
#define LJ_GC2_HS_EXIT_TRACES		0x00000080u
#define LJ_GC2_HS_REDISPATCH		0x00000100u
#define LJ_GC2_HS_FLUSHJ		0x00000200u
#define LJ_GC2_HS_STOPREQ		0x00000400u

#define LJ_GC2_ACCT_FLUSH		32768u
#define LJ_GC2_WORKER_DRAIN_BATCH	64u
#define LJ_GC2_WEAK_DRAIN_BATCH		64u
#define LJ_GC2_SWEEP_BATCH		64u
#define LJ_GC2_MINOR_SURVIVAL_MAJOR_PCT	80u

LJ_FUNC void lj_gc2_init(global_State *g);
LJ_FUNC void lj_gc2_fini(global_State *g);
LJ_FUNC void lj_gc2_account_alloc(global_State *g, TGState *tg, GCSize bytes);
LJ_FUNC uint64_t lj_gc2_flush_alloc(global_State *g, TGState *tg);
LJ_FUNC void lj_gc2_update_pacing(global_State *g);
LJ_FUNC uint32_t lj_gc2_assist_shift_from_stepmul(uint32_t stepmul);
LJ_FUNC uint32_t lj_gc2_assist(global_State *g, TGState *tg);
LJ_FUNC void lj_gc2_set_generational(global_State *g, int enabled);
LJ_FUNC void lj_gc2_legacy_mark_begin(global_State *g);
LJ_FUNC void lj_gc2_force_major(global_State *g);
LJ_FUNC void lj_gc2_update_minor_survival_policy(global_State *g, uint64_t live);
LJ_FUNC void lj_gc2_legacy_weak_begin(global_State *g);
LJ_FUNC void lj_gc2_legacy_sweep_begin(global_State *g);
LJ_FUNC void lj_gc2_legacy_preserve_abort(global_State *g);
LJ_FUNC void lj_gc2_legacy_cycle_end(global_State *g);
LJ_FUNC int lj_gc2_sweep_to_idle(global_State *g);
LJ_FUNC uint64_t lj_gc2_sweep_live_aggregate(global_State *g);
LJ_FUNC int lj_gc2_sweep_tg_ready(TGState *tg);
LJ_FUNC int lj_gc2_sweep_needs_prepare(global_State *g);
LJ_FUNC int lj_gc2_sweep_pending(global_State *g);
LJ_FUNC uint32_t lj_gc2_handshake(global_State *g, uint32_t actions);
LJ_FUNC uint32_t lj_gc2_reclaim_retired(global_State *g, uint64_t epoch);
LJ_FUNC void lj_gc2_scan_roots(global_State *g, lua_State *L);
LJ_FUNC void lj_gc2_scan_minor_roots(global_State *g, lua_State *L);
LJ_FUNC void lj_gc2_scan_cycle_roots(global_State *g, lua_State *L);
LJ_FUNC int lj_gc2_ssb_push(global_State *g, GCobj *o);
LJ_FUNC uint32_t lj_gc2_flush_ssb(global_State *g, TGState *tg);
LJ_FUNC uint32_t lj_gc2_drain_ssb(global_State *g);
LJ_FUNC GCobj *lj_gc2_grey_steal(global_State *g);
LJ_FUNC int lj_gc2_worker_start(global_State *g);
LJ_FUNC int lj_gc2_workers_set(global_State *g, uint32_t n);
LJ_FUNC void lj_gc2_worker_stop(global_State *g);
LJ_FUNC void lj_gc2_worker_wake(global_State *g);
LJ_FUNC uint32_t lj_gc2_worker_drain(global_State *g, uint32_t limit);
LJ_FUNC uint32_t lj_gc2_sweep_owner_progress(global_State *g, TGState *tg,
					     uint32_t limit);
LJ_FUNC uint32_t lj_gc2_fixpoint_round(global_State *g, lua_State *L,
				       uint32_t limit);
LJ_FUNC uint32_t lj_gc2_fixpoint_run(global_State *g, lua_State *L,
				     uint32_t max_rounds, uint32_t limit);
LJ_FUNC uint32_t lj_gc2_mark_complete(global_State *g, lua_State *L,
				      uint32_t max_rounds, uint32_t limit);
LJ_FUNC void lj_gc2_mark_to_weak(global_State *g);
LJ_FUNC int lj_gc2_weak_complete(global_State *g, GCobj *legacy,
				 uint32_t drain_limit);
LJ_FUNC void lj_gc2_weak_to_sweep(global_State *g);
LJ_FUNC int lj_gc2_ssb_empty(global_State *g);
LJ_FUNC uint32_t lj_gc2_weak_snapshot_count(global_State *g);
LJ_FUNC GCtab *lj_gc2_weak_snapshot_tab(global_State *g, uint32_t idx);
LJ_FUNC uint32_t lj_gc2_weak_snapshot_scan(global_State *g, uint32_t limit);
LJ_FUNC uint32_t lj_gc2_weak_snapshot_clear(global_State *g, uint32_t limit);
LJ_FUNC uint32_t lj_gc2_weak_drain(global_State *g, uint32_t limit);
LJ_FUNC int lj_gc2_weak_snapshot_covers_legacy(global_State *g,
					       GCobj *legacy);
LJ_FUNC void lj_gc2_weak_legacy_result(global_State *g, int skipped);
LJ_FUNC void lj_gc2_finreg_cdata_set(global_State *g, GCobj *o, int enabled);
LJ_FUNC void lj_gc2_finreg_cdata_queue(global_State *g, GCobj *o);
LJ_FUNC int lj_gc2_finreg_cdata_preclaim(lua_State *L, global_State *g,
					 GCobj *o, cTValue *fin);
LJ_FUNC int lj_gc2_finreg_cdata_preclaim_take(lua_State *L, global_State *g,
					      GCobj *o, TValue *fin);
#if defined(LUA_USE_ASSERT) || LJ_GC2_PARANOIA
LJ_FUNC void lj_gc2_test_finreg_cdata_preclaim_fail(global_State *g,
								    uint32_t n);
LJ_FUNC void lj_gc2_test_finreg_cdata_preclaim_publish_pause(global_State *g);
LJ_FUNC void lj_gc2_test_finalizer_drain_pause(global_State *g);
#endif
LJ_FUNC int lj_gc2_finreg_udata_set(global_State *g, GCobj *o, int enabled);
LJ_FUNC void lj_gc2_finreg_udata_register(lua_State *L, global_State *g,
					  GCobj *o);
LJ_FUNC void lj_gc2_finreg_udata_register_mt(lua_State *L, global_State *g,
					     GCudata *ud, GCtab *mt);
LJ_FUNC void lj_gc2_finreg_udata_forget(global_State *g, GCobj *o);
LJ_FUNC void lj_gc2_finreg_udata_queue(global_State *g, GCobj *o);
LJ_FUNC void lj_gc2_finalizer_enqueue(global_State *g, GCobj *o);
LJ_FUNC void lj_gc2_finalizer_drain_owned(global_State *g);
LJ_FUNC void lj_gc2_finalizer_drain(global_State *g);
LJ_FUNC GCobj *lj_gc2_finalizer_dequeue_owned(global_State *g);
LJ_FUNC GCobj *lj_gc2_finalizer_dequeue(global_State *g);
LJ_FUNC int lj_gc2_finalizer_try_enter(global_State *g);
LJ_FUNC void lj_gc2_finalizer_enter(global_State *g);
LJ_FUNC void lj_gc2_finalizer_leave(global_State *g);
LJ_FUNC int lj_gc2_finalizer_queue_pending(global_State *g);
LJ_FUNC int lj_gc2_finalizer_pending(global_State *g);
LJ_FUNC int lj_gc2_finalizer_sweep_pending(global_State *g);
LJ_FUNC void lj_gc2_barrier_tv(lua_State *L, cTValue *tv);
LJ_FUNCA void lj_gc2_barrier_tv_g(global_State *g, cTValue *tv);
LJ_FUNCA void lj_gc2_barrier_tvn_g(global_State *g, cTValue *tv, uint32_t n);
LJ_FUNCA void lj_gc2_barrier_tvn_pair_g(global_State *g, GCobj *parent,
					cTValue *tv, uint32_t n);
LJ_FUNC void lj_gc2_barrier_uv(global_State *g, cTValue *tv);
LJ_FUNC void lj_gc2_barrier_obj(lua_State *L, GCobj *o);
LJ_FUNCA void lj_gc2_barrier_obj_pair(lua_State *L, GCobj *parent, GCobj *child);
LJ_FUNCA void lj_gc2_barrier_tv_pair_g(global_State *g, GCobj *parent,
				       cTValue *tv);
LJ_FUNC void lj_gc2_barrier_tv_pair(lua_State *L, GCobj *parent, cTValue *tv);
LJ_FUNCA void lj_gc2_barrier_tab_g(global_State *g, GCtab *t);
LJ_FUNC void lj_gc2_barrier_tab(lua_State *L, GCtab *t);
LJ_FUNC int lj_gc2_markobj(global_State *g, GCobj *o);
LJ_FUNC int lj_gc2_markmem(global_State *g, void *p);
LJ_FUNC int lj_gc2_ismarkedmem(global_State *g, void *p);
LJ_FUNC int lj_gc2_ismarked(global_State *g, GCobj *o);
#if LJ_GC2_PARANOIA
LJ_FUNC uint32_t lj_gc2_paranoia_legacy_diff(global_State *g);
#endif

#endif
