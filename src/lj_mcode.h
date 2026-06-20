/*
** Machine code management.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_MCODE_H
#define _LJ_MCODE_H

#include "lj_obj.h"

#if LJ_HASJIT || LJ_HASFFI
LJ_FUNC void lj_mcode_sync(void *start, void *end);
#endif

#if LJ_HASJIT

#include "lj_jit.h"

LJ_FUNC void lj_mcode_init(global_State *g);
LJ_FUNC void lj_mcode_sync_core(jit_State *J);
LJ_FUNC void lj_mcode_free(jit_State *J);
LJ_FUNC void lj_mcode_freeall(global_State *g);
LJ_FUNC uint32_t lj_mcode_reclaim_retired(global_State *g,
					  uint64_t completed_epoch);
LJ_FUNC void lj_mcode_freeretired(global_State *g);
LJ_FUNC void lj_mcode_markretired(global_State *g, int gc2);
LJ_FUNC MCode *lj_mcode_reserve(jit_State *J, MCode **lim);
LJ_FUNC void lj_mcode_commit(jit_State *J, MCode *m);
LJ_FUNC void lj_mcode_abort(jit_State *J);
LJ_FUNC MCode *lj_mcode_patch(jit_State *J, MCode *ptr, int finish);
LJ_FUNC_NORET void lj_mcode_limiterr(jit_State *J, size_t need);

static LJ_AINLINE MCode *lj_mcode_area_rw(MCode *area)
{
  return ((MCLink *)area)->rw;
}

static LJ_AINLINE MCode *lj_mcode_rx2rw(MCode *area, MCode *rx)
{
  return (MCode *)((char *)lj_mcode_area_rw(area) +
		  ((char *)rx - (char *)area));
}

static LJ_AINLINE MCode *lj_mcode_rw2rx(MCode *area, MCode *rw)
{
  return (MCode *)((char *)area +
		  ((char *)rw - (char *)lj_mcode_area_rw(area)));
}

static LJ_AINLINE MCode *lj_mcode_rw(jit_State *J, MCode *rx)
{
  MCode *area = J->mcarea;
  while (area) {
    size_t sz = ((MCLink *)area)->size;
    if (rx >= area && rx < (MCode *)((char *)area + sz))
      return lj_mcode_rx2rw(area, rx);
    area = mcode_area_next_acq(area);
  }
  return rx;
}

#define lj_mcode_commitbot(J, m)	(J->mcbot = (m))

#else

#define lj_mcode_init(g)		UNUSED(g)
#define lj_mcode_sync_core(J)		UNUSED(J)
#define lj_mcode_freeall(g)		UNUSED(g)
#define lj_mcode_reclaim_retired(g, e)	(UNUSED(g), UNUSED(e), 0)
#define lj_mcode_freeretired(g)		UNUSED(g)
#define lj_mcode_markretired(g, gc2)	(UNUSED(g), UNUSED(gc2))

#endif

#endif
