/*
** Trace management.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_TRACE_H
#define _LJ_TRACE_H

#include "lj_obj.h"

#if LJ_HASJIT
#include "lj_atomic.h"
#include "lj_jit.h"
#include "lj_dispatch.h"

/* Trace errors. */
typedef enum {
#define TREDEF(name, msg)	LJ_TRERR_##name,
#include "lj_traceerr.h"
  LJ_TRERR__MAX
} TraceError;

LJ_FUNC_NORET void lj_trace_err(jit_State *J, TraceError e);
LJ_FUNC_NORET void lj_trace_err_info(jit_State *J, TraceError e);

/* Trace management. */
LJ_FUNC GCtrace * LJ_FASTCALL lj_trace_alloc(lua_State *L, GCtrace *T);
LJ_FUNC void LJ_FASTCALL lj_trace_free(global_State *g, GCtrace *T);
LJ_FUNC void LJ_FASTCALL lj_trace_free_unpublished(global_State *g, GCtrace *T);
LJ_FUNC void lj_trace_reenableproto(GCproto *pt);
LJ_FUNC uint32_t lj_trace_flushproto(global_State *g, GCproto *pt);
LJ_FUNC uint32_t lj_trace_flush(jit_State *J, TraceNo traceno);
LJ_FUNC uint32_t lj_trace_flush_unlink(jit_State *J, TraceNo traceno);
LJ_FUNC BCIns LJ_FASTCALL lj_trace_stale_startins(jit_State *J,
						  const BCIns *pc,
						  TraceNo traceno,
						  lua_State *L);
LJ_FUNC uint32_t lj_trace_flushscope(jit_State *J, TraceNo traceno);
LJ_FUNC int lj_trace_hasany(global_State *g);
LJ_FUNC int lj_trace_flushall(lua_State *L);
LJ_FUNC int lj_trace_flushall_gc(lua_State *L);
LJ_FUNC int lj_trace_flushall_hs(lua_State *L);
LJ_FUNC void lj_trace_flushscope_hs(global_State *g, uint32_t work);
LJ_FUNC uint32_t lj_trace_flushscope_retire_hs(global_State *g,
					       uint64_t epoch);
LJ_FUNC int lj_jit_token_try(jit_State *J);
LJ_FUNC int lj_jit_token_held(jit_State *J);
LJ_FUNC int lj_jit_token_acquire_wait(jit_State *J);
LJ_FUNC void lj_jit_token_release(jit_State *J);
LJ_FUNC void lj_trace_abort(global_State *g);
LJ_FUNC void lj_trace_initstate(global_State *g);
LJ_FUNC void lj_trace_freestate(global_State *g);
LJ_FUNC uint32_t lj_trace_reclaim_retired(global_State *g,
					  uint64_t completed_epoch);
LJ_FUNC int lj_trace_retired_mcode_refs(global_State *g, MCode *area,
					size_t size);
LJ_FUNC void lj_trace_freeretired(global_State *g);
LJ_FUNC void lj_trace_markvecs(global_State *g, int gc2);

/* Event handling. */
LJ_FUNC void lj_trace_ins(jit_State *J, const BCIns *pc);
#if LJ_TARGET_X64
LJ_FUNCA void LJ_FASTCALL lj_trace_hot(jit_State *J, const BCIns *pc,
				       lua_State *L);
LJ_FUNCA uint32_t LJ_FASTCALL lj_trace_stitch_probe(jit_State *J, GCtrace *T);
LJ_FUNCA void LJ_FASTCALL lj_trace_stitch(jit_State *J, const BCIns *pc,
					  lua_State *L, TraceNo traceno);
#else
LJ_FUNCA void LJ_FASTCALL lj_trace_hot(jit_State *J, const BCIns *pc);
LJ_FUNCA void LJ_FASTCALL lj_trace_stitch(jit_State *J, const BCIns *pc);
#endif
#if LJ_TARGET_X64 && !LJ_ABI_WIN
LJ_FUNCA int LJ_FASTCALL lj_trace_exit(jit_State *J, void *exptr, lua_State *L,
				       TraceNo parent, ExitNo exitno);
#else
LJ_FUNCA int LJ_FASTCALL lj_trace_exit(jit_State *J, void *exptr);
#endif
#if LJ_UNWIND_EXT
LJ_FUNC uintptr_t LJ_FASTCALL lj_trace_unwind(jit_State *J, uintptr_t addr, ExitNo *ep);
#endif

/* Signal asynchronous abort of trace or end of trace. */
static LJ_AINLINE TraceState lj_trace_state_load(jit_State *J)
{
  return (TraceState)la_load32_acq((uint32_t *)&J->state);
}

static LJ_AINLINE void lj_trace_state_store(jit_State *J, TraceState st)
{
  la_store32_rel((uint32_t *)&J->state, (uint32_t)st);
}

static LJ_AINLINE int lj_trace_state_aborted(TraceState st)
{
  uint32_t s = (uint32_t)st;
  return s != (uint32_t)LJ_TRACE_IDLE &&
	 (s & (uint32_t)LJ_TRACE_ACTIVE) == 0;
}

static LJ_AINLINE TraceState lj_trace_state_store_active(jit_State *J,
							 TraceState st)
{
  uint32_t old = la_load32_acq((uint32_t *)&J->state);
  for (;;) {
    uint32_t next = (uint32_t)st;
    if (old != (uint32_t)LJ_TRACE_IDLE &&
	(old & (uint32_t)LJ_TRACE_ACTIVE) == 0)
      next &= ~(uint32_t)LJ_TRACE_ACTIVE;
    if (la_cas32((uint32_t *)&J->state, &old, next, LA_ACQ_REL, LA_ACQ))
      return (TraceState)next;  /* 08 section 8.7: preserve async aborts. */
  }
}

static LJ_AINLINE void lj_trace_state_abort(jit_State *J)
{
  uint32_t old = (uint32_t)lj_trace_state_load(J);
  while ((old & (uint32_t)LJ_TRACE_ACTIVE) != 0) {
    uint32_t next = old & ~(uint32_t)LJ_TRACE_ACTIVE;
    if (la_cas32((uint32_t *)&J->state, &old, next, LA_ACQ_REL, LA_ACQ))
      break;  /* 08 section 8.7: publish async recorder abort. */
  }
}

#define lj_trace_end(J)		lj_trace_state_store_active((J), LJ_TRACE_END)

#else

#define lj_trace_flushall(L)	(UNUSED(L), 0)
#define lj_trace_flushall_hs(L)	(UNUSED(L), 0)
#define lj_trace_hasany(g)	(UNUSED(g), 0)
#define lj_jit_token_try(J)	(UNUSED(J), 0)
#define lj_jit_token_held(J)	(UNUSED(J), 0)
#define lj_jit_token_acquire_wait(J)	(UNUSED(J), 0)
#define lj_jit_token_release(J)	UNUSED(J)
#define lj_trace_initstate(g)	UNUSED(g)
#define lj_trace_freestate(g)	UNUSED(g)
#define lj_trace_reclaim_retired(g, e)	(UNUSED(g), UNUSED(e), 0)
#define lj_trace_retired_mcode_refs(g, area, size) \
  (UNUSED(g), UNUSED(area), UNUSED(size), 0)
#define lj_trace_freeretired(g)	UNUSED(g)
#define lj_trace_markvecs(g, gc2)	(UNUSED(g), UNUSED(gc2))
#define lj_trace_abort(g)	UNUSED(g)
#define lj_trace_end(J)		UNUSED(J)

#endif

#endif
