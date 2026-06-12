/*
** Per-OS-thread state block scaffolding for LuaJIT-MT.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_TG_H
#define _LJ_TG_H

#include "lj_obj.h"
#include "lj_bc.h"
#include "lj_arena.h"

/* Type of hot counter. Must match the code in the assembler VM. */
/* 16 bits are sufficient. Only 0.0015% overhead with maximum slot penalty. */
typedef uint16_t HotCount;

/* Number of hot counter hash table entries (must be a power of two). */
#define HOTCOUNT_SIZE		64
#define HOTCOUNT_PCMASK		((HOTCOUNT_SIZE-1)*sizeof(HotCount))

/* Hotcount decrements. */
#define HOTCOUNT_LOOP		2
#define HOTCOUNT_CALL		1

/* This solves a circular dependency problem -- bump as needed. Sigh. */
#define GG_NUM_ASMFF	57

#define GG_LEN_DDISP	(BC__MAX + GG_NUM_ASMFF)
#define GG_LEN_SDISP	BC_FUNCF
#define GG_LEN_DISP	(GG_LEN_DDISP + GG_LEN_SDISP)

#define TGF_ARENA_INTERNAL	0x01u
#define TGF_HUGETAB		0x02u
#define TGF_DEAD		0x04u
#define TGF_STOPREQ		0x08u
#define TG_HUGETAB_BITS		16u
#define TG_GC2_SSB_BYTES	1024u
#define TG_GC2_SSB_SLOTS	(TG_GC2_SSB_BYTES / sizeof(GCRef))

typedef struct GG_State GG_State;
typedef struct ExitTrampolines ExitTrampolines;

struct GC2SSBNode {
  GC2SSBNode *next;
  TGState *owner;
  uint32_t n;
  uint32_t pad;
  GCRef slot[TG_GC2_SSB_SLOTS];
};

struct TGState {
  HotCount hotcount[HOTCOUNT_SIZE];
  ASMFunction dispatch[GG_LEN_DISP];
  uint32_t poll;
  uint32_t mark_active;
  global_State *gl;
  lua_State *cur_L;
  TValue *jit_base;
  uint8_t in_native;
  uint8_t gc_assist;
  uint8_t hookmask_th;
  uint8_t tg_flags;
  uint32_t reqmask;
  uint64_t hs_epoch_ack;
  TGAlloc alloc;
  LJArenaAllocD allocd;
  HugeTab huge;
  GC2SSBNode ssb_node[2];
  GC2SSBNode *ssb_active, *ssb_free;
  GCRef *ssb_next, *ssb_end, *ssb_base;
  SBuf tmpbuf;
  TValue tmptv, tmptv2;
  PRNGState prng;
  lua_State *thread_L;
  GCudata *thread_ud;
  uint32_t tid;
  TGState *next_tg;
  uint64_t local_total;
  uint64_t stack_dirty_epoch;
  ExitTrampolines *exittr;
};

LJ_STATIC_ASSERT(sizeof(((GC2SSBNode *)0)->slot) == TG_GC2_SSB_BYTES);

static LJ_AINLINE lua_State *lj_tg_cur_L(global_State *g)
{
  TGState *tg = lj_thr_get_tg();
  if (tg)
    return tg->cur_L;
  return gcref(g->cur_L) ? gco2th(gcref(g->cur_L)) : NULL;
}

static LJ_AINLINE void lj_tg_setcur_L(global_State *g, lua_State *L)
{
  TGState *tg = G2TG(g);
  if (tg)
    tg->cur_L = L;
  setgcref(g->cur_L, obj2gco(L));  /* Transitional mirror for VM asm. */
}

static LJ_AINLINE void lj_tg_clearcur_L(global_State *g)
{
  TGState *tg = G2TG(g);
  if (tg)
    tg->cur_L = NULL;
  setgcrefnull(g->cur_L);  /* Transitional mirror for VM asm. */
}

static LJ_AINLINE TValue *lj_tg_jit_base(global_State *g)
{
  TGState *tg = lj_thr_get_tg();
  if (tg)
    return tg->jit_base;
  return tvref(g->jit_base);  /* Transitional mirror for VM asm writes. */
}

static LJ_AINLINE void lj_tg_setjit_base(global_State *g, TValue *base)
{
  TGState *tg = G2TG(g);
  if (tg)
    tg->jit_base = base;
  setmref(g->jit_base, base);  /* Transitional mirror for VM asm. */
}

#define TG_DISP2HOT	(-(int)(HOTCOUNT_SIZE*sizeof(HotCount)))
#define TG_OFS(f) \
  ((int)(offsetof(TGState, f) - offsetof(TGState, dispatch)))
#define DISPATCH_TG(f)	TG_OFS(f)

LJ_FUNC void lj_tg_init(GG_State *GG, int alloc_ready);
LJ_FUNC void lj_tg_fini(global_State *g);
LJ_FUNC void lj_tg_init_thread(global_State *g, TGState *tg, lua_State *L,
			       int arena_internal);
LJ_FUNC void lj_tg_fini_thread(global_State *g, TGState *tg);
LJ_FUNC void lj_tg_attach(global_State *g, TGState *tg);
LJ_FUNC void lj_tg_detach(global_State *g, TGState *tg);
LJ_FUNC uint32_t lj_tg_reclaim_dead(global_State *g);
LJ_FUNC void lj_tg_sync_dispatch_tg(global_State *g, TGState *tg);
LJ_FUNC void lj_tg_sync_dispatch(global_State *g);

#endif
