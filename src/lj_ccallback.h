/*
** FFI C callback handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_CCALLBACK_H
#define _LJ_CCALLBACK_H

#include "lj_obj.h"
#include "lj_ctype.h"

#if LJ_HASFFI

/* Really belongs to lj_vm.h. */
LJ_ASMF void lj_vm_ffi_callback(void);

LJ_FUNC MSize lj_ccallback_ptr2slot(CTState *cts, void *p);
LJ_FUNCA CCallbackRuntime * LJ_FASTCALL lj_ccallback_prepare(CTState *cts,
							     MSize slot);
LJ_FUNCA lua_State * LJ_FASTCALL lj_ccallback_enter(CTState *cts, void *cf,
						    CCallbackRuntime *cb);
LJ_FUNCA void LJ_FASTCALL lj_ccallback_leave(CTState *cts, TValue *o,
					     CCallbackRuntime *cb);
/* The supported MT backends must copy TG-owned result carriers before the
** matching finish call releases a foreign callback's auto-attached TG. */
LJ_FUNCA lua_State * LJ_FASTCALL lj_ccallback_leave_result(
  CTState *cts, TValue *o, CCallbackRuntime *cb);
LJ_FUNCA void LJ_FASTCALL lj_ccallback_leave_result_finish(lua_State *L);
#ifdef LJ_CCALLBACK_TEST_HELPERS
typedef void (*LJCCallbackAfterDetachHook)(void);
LJ_FUNC void lj_ccallback_test_set_after_detach_hook(
  LJCCallbackAfterDetachHook hook);
#endif
LJ_FUNC void lj_ccallback_unwind(lua_State *L, TValue *cont);
LJ_FUNC void lj_ccallback_unwind_detach(void);
LJ_FUNC void lj_ccallback_disown_state(lua_State *L);
LJ_FUNC MSize lj_ccallback_maxslot(void);
LJ_FUNC void lj_ccallback_init_l(lua_State *L, CTState *cts);
LJ_FUNC void *lj_ccallback_new_l(lua_State *L, CTState *cts, CTypeID id,
				 GCfunc *fn);
LJ_FUNC void lj_ccallback_func_store_l(lua_State *L, CTState *cts,
				       MSize slot, GCfunc *fn);
LJ_FUNC void lj_ccallback_func_clear(CTState *cts, MSize slot);
LJ_FUNC void lj_ccallback_mcode_free(CTState *cts);

#else

#define lj_ccallback_disown_state(L)	UNUSED(L)

#endif

#endif
