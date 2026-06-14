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
LJ_FUNCA lua_State * LJ_FASTCALL lj_ccallback_enter(CTState *cts, void *cf,
						    CCallbackRuntime *cb);
LJ_FUNCA void LJ_FASTCALL lj_ccallback_leave(CTState *cts, TValue *o,
					     CCallbackRuntime *cb);
LJ_FUNC void lj_ccallback_unwind(lua_State *L, TValue *cont);
LJ_FUNC MSize lj_ccallback_maxslot(void);
LJ_FUNC void lj_ccallback_init_l(lua_State *L, CTState *cts);
LJ_FUNC void *lj_ccallback_new_l(lua_State *L, CTState *cts, CType *ct,
				 GCfunc *fn);
LJ_FUNC void lj_ccallback_mcode_free(CTState *cts);

#endif

#endif
