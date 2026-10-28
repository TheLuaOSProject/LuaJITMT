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
#include "lj_trace.h"

/* Keep the incoming values of a pure Lua CALL across recorder/LOOP merges.
** This only retains evidence. Neither admission pass trusts this predicate,
** and effectful C/FFI calls retain their existing continuation policy. */
static LJ_AINLINE int lj_snap_lua_call(const jit_State *J, const SnapShot *snap)
{
  const GCtrace *T = &J->cur;
  GCobj *o = trace_startptgco_acq((GCtrace *)T);
  const GCproto *pt;
  uintptr_t pc, lo;
  uint64_t pcbase;
  BCIns call;
  MSize i, slot;
  IRRef fn = 0, r;
  if (bc_op(T->startins) != BC_FORL || !o || !checkptrGC(o) ||
      o->gch.gct != (uint32_t)~LJ_TPROTO ||
      snap->mapofs > T->nsnapmap ||
      (MSize)snap->nent+2u > T->nsnapmap-snap->mapofs) return 0;
  pt = gco2pt(o); lo = (uintptr_t)proto_bc(pt);
  memcpy(&pcbase, &T->snapmap[snap->mapofs+snap->nent], sizeof(pcbase));
  pc = (uintptr_t)(pcbase>>8);
  if ((uint8_t)pcbase || pc < lo || (pc-lo) % sizeof(BCIns) ||
      (pc-lo) / sizeof(BCIns) >= pt->sizebc) return 0;
  call = (BCIns)la_load32_acq((const uint32_t *)pc);
  if (bc_op(call) != BC_CALL || bc_b(call) != 2 || !bc_c(call) ||
      bc_a(call)+bc_c(call)+LJ_FR2 > pt->framesize) return 0;
  slot = 1u+LJ_FR2+bc_a(call);
  for (i = 0; i < snap->nent; i++) {
    SnapEntry entry = T->snapmap[snap->mapofs+i];
    if (snap_slot(entry) == slot && !(entry & 0x00ff0000u))
      fn = snap_ref(entry);
  }
  if (fn < T->nk || fn >= T->nins || irt_type(T->ir[fn].t) != IRT_FUNC)
    return 0;
  if (!irref_isk(fn)) {
    for (r = fn+1u; r <= snap->ref && r < T->nins; r++) {
      const IRIns *guard = &T->ir[r];
      if (guard->o == IR_EQ && guard->t.irt == (IRT_FUNC|IRT_GUARD) &&
          guard->op1 == fn && guard->op2 >= T->nk &&
          guard->op2 < REF_TRUE && T->ir[guard->op2].o == IR_KGC) {
        fn = guard->op2; break;
      }
    }
  }
  if (!irref_isk(fn) || T->ir[fn].o != IR_KGC) return 0;
  o = ir_kgc_acq(&T->ir[fn]);
  return o && checkptrGC(o) && o->gch.gct == (uint32_t)~LJ_TFUNC &&
         lj_trace_arm64_lua_leaf(gco2func(o));
}

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
     call->op2 == IRCALL_lj_func_storeuv_rooted_try ||
     call->op2 == IRCALL_lj_func_storecell_rooted_try);
}
#else
#define lj_snap_lua_call(J, snap) 0
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
