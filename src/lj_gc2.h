/*
** Concurrent GC scaffold.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_GC2_H
#define _LJ_GC2_H

#include "lj_obj.h"

enum {
  LJ_GC2_IDLE,
  LJ_GC2_MARK,
  LJ_GC2_WEAK,
  LJ_GC2_SWEEP
};

LJ_FUNC void lj_gc2_init(global_State *g);
LJ_FUNC void lj_gc2_legacy_mark_begin(global_State *g);
LJ_FUNC void lj_gc2_legacy_sweep_begin(global_State *g);
LJ_FUNC void lj_gc2_legacy_cycle_end(global_State *g);
LJ_FUNC int lj_gc2_markobj(global_State *g, GCobj *o);
LJ_FUNC int lj_gc2_markmem(global_State *g, void *p);
LJ_FUNC int lj_gc2_ismarked(global_State *g, GCobj *o);

#endif
