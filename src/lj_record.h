/*
** Trace recorder (bytecode -> SSA IR).
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_RECORD_H
#define _LJ_RECORD_H

#include "lj_obj.h"
#include "lj_jit.h"

#if LJ_HASJIT
static LJ_AINLINE int lj_record_mt_runtime_shared(global_State *g,
						  lua_State *L)
{
  TGState *tg = L ? L2TG(L) : G2TG(g);
  /*
  ** mt_active is the steady-state latch, but recording can also happen while a
  ** secondary TG is live/entering or already linked into the GC2 thread set.
  ** A worker TG may record while those counters are between publication windows;
  ** if the current recorder does not belong to the main TG, generated code must
  ** use the active-MT table and frame rules instead of stock single-mutator
  ** shortcuts.
  */
  return (tg != NULL && tg != g->main_tg) ||
	 mt_active_or_entering_acq(g) || mt_live_acq(g) != 0 ||
	 gc2_n_threads_acq(g) > 1;
}

/* Context for recording an indexed load/store. */
typedef struct RecordIndex {
  TValue tabv;		/* Runtime value of table (or indexed object). */
  TValue keyv;		/* Runtime value of key. */
  TValue valv;		/* Runtime value of stored value. */
  TValue mobjv;		/* Runtime value of metamethod object. */
  GCtab *mtv;		/* Runtime value of metatable object. */
  cTValue *oldv;	/* Runtime value of previously stored value. */
  TRef tab;		/* Table (or indexed object) reference. */
  TRef key;		/* Key reference. */
  TRef val;		/* Value reference for a store or 0 for a load. */
  TRef mt;		/* Metatable reference. */
  TRef mobj;		/* Metamethod object reference. */
  int idxchain;		/* Index indirections left or 0 for raw lookup. */
} RecordIndex;

LJ_FUNC int lj_record_objcmp(jit_State *J, TRef a, TRef b,
			     cTValue *av, cTValue *bv);
LJ_FUNC void lj_record_stop(jit_State *J, TraceLink linktype, TraceNo lnk);
LJ_FUNC TRef lj_record_constify(jit_State *J, cTValue *o);
LJ_FUNC TRef lj_record_vload(jit_State *J, TRef ref, MSize idx, IRType t);

LJ_FUNC void lj_record_call(jit_State *J, BCReg func, ptrdiff_t nargs);
LJ_FUNC void lj_record_tailcall(jit_State *J, BCReg func, ptrdiff_t nargs);
LJ_FUNC void lj_record_ret(jit_State *J, BCReg rbase, ptrdiff_t gotresults);

LJ_FUNC int lj_record_mm_lookup(jit_State *J, RecordIndex *ix, MMS mm);
LJ_FUNC TRef lj_record_idx(jit_State *J, RecordIndex *ix);
LJ_FUNC int lj_record_mt_shared_tab(jit_State *J, TRef tab);
LJ_FUNC int lj_record_next(jit_State *J, RecordIndex *ix);

LJ_FUNC void lj_record_ins(jit_State *J);
LJ_FUNC void lj_record_setup(jit_State *J, BCIns root_iterl);
#endif

#endif
