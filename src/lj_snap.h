/*
** Snapshot handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_SNAP_H
#define _LJ_SNAP_H

#include "lj_obj.h"
#include "lj_jit.h"

#if LJ_HASJIT
#if LJ_TARGET_ARM64 && defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL)
#include "lj_ircall.h"

/* Retain the exact continuation after a rooted store effect/status pair.
** DONE alone also marks FFI boundaries, whose existing merging is unchanged.
** This is a recorder retention rule; admission separately proves the call. */
static LJ_AINLINE int lj_snap_rooted_continuation(const jit_State *J,
                                                const SnapShot *snap)
{
  const IRIns *call;
  if (snap->count != SNAPCOUNT_DONE || snap->ref < REF_FIRST+2u ||
      snap->ref > J->cur.nins)
    return 0;
  call = &J->cur.ir[snap->ref-2u];
  return call->o == IR_CALLS &&
    (call->op2 == IRCALL_lj_tab_storeraw_rooted ||
     call->op2 == IRCALL_lj_tab_storenil_rooted ||
     call->op2 == IRCALL_lj_func_storeuv_rooted_try);
}
#else
#define lj_snap_rooted_continuation(J, snap) 0
#endif

LJ_FUNC void lj_snap_add(jit_State *J);
LJ_FUNC void lj_snap_purge(jit_State *J);
LJ_FUNC void lj_snap_shrink(jit_State *J);
LJ_FUNC IRIns *lj_snap_regspmap(jit_State *J, GCtrace *T, SnapNo snapno,
				IRIns *ir);
LJ_FUNC void lj_snap_replay(jit_State *J, GCtrace *T);
LJ_FUNC const BCIns *lj_snap_restore(jit_State *J, void *exptr);
#if LJ_TARGET_X64 || LJ_TARGET_ARM64
LJ_FUNC const BCIns *lj_snap_restore_exit(jit_State *J, void *exptr,
					  lua_State *L, GCtrace *T,
					  TraceNo parent,
					  ExitNo exitno);
#endif
LJ_FUNC void lj_snap_grow_buf_(jit_State *J, MSize need);
LJ_FUNC void lj_snap_grow_map_(jit_State *J, MSize need);

static LJ_AINLINE void lj_snap_grow_buf(jit_State *J, MSize need)
{
  if (LJ_UNLIKELY(need > J->sizesnap)) lj_snap_grow_buf_(J, need);
}

static LJ_AINLINE void lj_snap_grow_map(jit_State *J, MSize need)
{
  if (LJ_UNLIKELY(need > J->sizesnapmap)) lj_snap_grow_map_(J, need);
}

#endif

#endif
