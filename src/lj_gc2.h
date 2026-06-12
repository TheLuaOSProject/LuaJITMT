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

LJ_FUNC void lj_gc2_init(global_State *g);
LJ_FUNC void lj_gc2_legacy_mark_begin(global_State *g);
LJ_FUNC void lj_gc2_legacy_sweep_begin(global_State *g);
LJ_FUNC void lj_gc2_legacy_preserve_abort(global_State *g);
LJ_FUNC void lj_gc2_legacy_cycle_end(global_State *g);
LJ_FUNC uint32_t lj_gc2_handshake(global_State *g, uint32_t actions);
LJ_FUNC int lj_gc2_markobj(global_State *g, GCobj *o);
LJ_FUNC int lj_gc2_markmem(global_State *g, void *p);
LJ_FUNC int lj_gc2_ismarkedmem(global_State *g, void *p);
LJ_FUNC int lj_gc2_ismarked(global_State *g, GCobj *o);
#if LJ_GC2_PARANOIA
LJ_FUNC uint32_t lj_gc2_paranoia_legacy_diff(global_State *g);
#endif

#endif
