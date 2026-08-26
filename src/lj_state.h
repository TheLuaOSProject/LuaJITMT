/*
** State and stack handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_STATE_H
#define _LJ_STATE_H

#include "lj_obj.h"

typedef struct LJGC2Lease LJGC2Lease;

#define incr_top(L) \
  (++L->top >= tvref(L->maxstack) && (lj_state_growstack1(L), 0))

#define savestack(L, p)		((char *)(p) - mref(L->stack, char))
#define restorestack(L, n)	((TValue *)(mref(L->stack, char) + (n)))

LJ_FUNC void lj_state_relimitstack(lua_State *L);
LJ_FUNC void lj_state_shrinkstack(lua_State *L, MSize used);
LJ_FUNCA void LJ_FASTCALL lj_state_growstack(lua_State *L, MSize need);
LJ_FUNC void LJ_FASTCALL lj_state_growstack1(lua_State *L);
LJ_FUNC int LJ_FASTCALL lj_state_cpgrowstack(lua_State *L, MSize need);
LJ_FUNC int lj_state_rehome_stack(lua_State *L);
LJ_FUNCA void lj_state_stack_dirty_vm(lua_State *L);
LJ_FUNCA void lj_state_stack_pubtv(lua_State *L, lua_State *target,
				  cTValue *tv);
LJ_FUNC void lj_state_stack_pubrange(lua_State *L, lua_State *target);

static LJ_AINLINE void lj_state_checkstack(lua_State *L, MSize need)
{
  if ((mref(L->maxstack, char) - (char *)L->top) <=
      (ptrdiff_t)need*(ptrdiff_t)sizeof(TValue))
    lj_state_growstack(L, need);
}

/* Returns with the new thread held in one TG root-anchor slot. The caller must
** publish its permanent root and pop anchoridx without an intervening throw. */
LJ_FUNC lua_State *lj_state_new_withenv(lua_State *L, GCtab *env,
					 uint32_t *anchoridx);
LJ_FUNC lua_State *lj_state_new_withenv_envrooted(lua_State *L, GCtab *env,
						 uint32_t anchoridx);
LJ_FUNC lua_State *lj_state_new(lua_State *L, uint32_t *anchoridx);
LJ_FUNC void LJ_FASTCALL lj_state_free(global_State *g, lua_State *L);

enum {
  LJ_STATE_GCPREP_NONE = 0,
  LJ_STATE_GCPREP_PENDING = 1,
  LJ_STATE_GCPREP_DONE = 2
};

static LJ_AINLINE uint32_t lj_state_gcprep_state_acq(const lua_State *L)
{
  return la_load32_acq(&L->gcprep_state);
}

static LJ_AINLINE void lj_state_gcprep_state_store_rlx(lua_State *L,
							uint32_t state)
{
  la_store32_rlx(&L->gcprep_state, state);
}

static LJ_AINLINE void lj_state_gcprep_state_rel(lua_State *L,
						  uint32_t state)
{
  la_store32_rel(&L->gcprep_state, state);
}

static LJ_AINLINE lua_State *lj_state_gcprep_next_acq(const lua_State *L)
{
  return (lua_State *)la_loadptr_acq((void *const *)&L->gcprep_next);
}

static LJ_AINLINE void lj_state_gcprep_next_rel(lua_State *L,
						 lua_State *next)
{
  la_storeptr_rel((void **)&L->gcprep_next, next);
}

static LJ_AINLINE uint32_t lj_state_gcprep_pending_acq(global_State *g)
{
  return la_load32_acq(&g->thread_gcprep_pending);
}

static LJ_AINLINE lua_State *lj_state_gcprep_head_acq(global_State *g)
{
  return (lua_State *)la_loadptr_acq((void *const *)&g->thread_gcprep);
}

static LJ_AINLINE int lj_state_gcprep_head_cas(global_State *g,
						lua_State **oldp,
						lua_State *head)
{
  return la_casptr((void **)&g->thread_gcprep, (void **)oldp, head,
		   LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE lua_State *lj_state_gcprep_head_xchg(global_State *g,
							lua_State *head)
{
  return (lua_State *)la_xchgptr_acqrel((void **)&g->thread_gcprep, head);
}

/* Exact-reclaimer terminal THREAD split. claim_and_pin() performs only root
** preflight, owner arbitration and the destructor-incomplete reservation; the
** caller must either cancel() on a lost destructor LP or publish() after
** lifetime FREE. drain() runs semantic preparation outside the exclusive
** writer and leaves physical bitmap reuse to the next exact arena pass. */
LJ_FUNC int lj_state_gcprep_claim_and_pin(global_State *g, lua_State *L);
LJ_FUNC void lj_state_gcprep_cancel(global_State *g, lua_State *L);
LJ_FUNC void lj_state_gcprep_publish(global_State *g, lua_State *L);
LJ_FUNC uint32_t lj_state_gcprep_drain(global_State *g, uint32_t limit);
LJ_FUNC void lj_state_gcprep_drain_terminal(global_State *g);

#if defined(LJ_GC2_TEST_HELPERS) || defined(LJ_STATE_TEST_HELPERS)
LJ_FUNC void lj_state_test_gcprep_pause(int enabled);
LJ_FUNC uint32_t lj_state_test_gcprep_paused(void);
LJ_FUNC void lj_state_test_gcprep_pre_lp_pause(int enabled);
LJ_FUNC uint32_t lj_state_test_gcprep_pre_lp_paused(void);
LJ_FUNC void lj_state_test_gcprep_terminal_drain_reset(void);
LJ_FUNC uint32_t lj_state_test_gcprep_terminal_drain_count(void);
#endif

LJ_FUNC int lj_state_thread_registry_lease(global_State *g, lua_State *th,
					    LJGC2Lease *lease);
LJ_FUNC int lj_state_thread_registry_valid(global_State *g, lua_State *th);
LJ_FUNC void lj_state_thread_registry_publish(global_State *g, lua_State *th);

static LJ_AINLINE lua_State *lj_state_thread_registry_head_acq(global_State *g)
{
  return (lua_State *)la_loadptr_acq((void *const *)&g->threading_states);
}

static LJ_AINLINE void lj_state_thread_registry_head_clear(global_State *g)
{
  la_storeptr_rlx((void **)&g->threading_states, NULL);
}

static LJ_AINLINE int
lj_state_thread_registry_head_cas(global_State *g, lua_State **oldp,
				  lua_State *head)
{
  return la_casptr((void **)&g->threading_states, (void **)oldp, head,
		   LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE lua_State *
lj_state_thread_registry_head_xchg(global_State *g, lua_State *head)
{
  return (lua_State *)la_xchgptr_acqrel((void **)&g->threading_states, head);
}

static LJ_AINLINE lua_State *lj_state_thread_registry_next_acq(lua_State *th)
{
  return (lua_State *)la_loadptr_acq((void *const *)&th->thread_next);
}

static LJ_AINLINE void lj_state_thread_registry_next_rel(lua_State *th,
							 lua_State *next)
{
  la_storeptr_rel((void **)&th->thread_next, next);
}

static LJ_AINLINE int
lj_state_thread_registry_next_cas(lua_State *th, lua_State **oldp,
				  lua_State *next)
{
  return la_casptr((void **)&th->thread_next, (void **)oldp, next,
		   LA_ACQ_REL, LA_ACQ);
}
#if LJ_64 && !LJ_GC64 && !(defined(LUAJIT_USE_VALGRIND) && defined(LUAJIT_USE_SYSMALLOC))
LJ_FUNC lua_State *lj_state_newstate(lua_Alloc f, void *ud);
#endif

#define LJ_ALLOCF_INTERNAL	((lua_Alloc)(void *)(uintptr_t)(1237<<4))

/*
** Temporary GC2 safety boundary: all state storage stays in the internal
** arena until arbitrary lua_Alloc callbacks have nonblocking body SMR,
** allocator-generation ownership and concurrent-callback semantics.
** Keep this as a named gate so the complete implementation can remove the
** policy without another public API or ABI change.
*/
#ifndef LJ_GC2_INTERNAL_ALLOCATOR_ONLY
#define LJ_GC2_INTERNAL_ALLOCATOR_ONLY 1
#endif

#endif
