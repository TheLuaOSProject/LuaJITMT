/*
** State and stack handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_STATE_H
#define _LJ_STATE_H

#include "lj_obj.h"

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
LJ_FUNC void lj_state_stack_pubtv(lua_State *L, lua_State *target,
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
