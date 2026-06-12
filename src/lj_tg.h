/*
** Per-OS-thread state block scaffolding for LuaJIT-MT.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_TG_H
#define _LJ_TG_H

#include "lj_obj.h"
#include "lj_bc.h"

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

typedef struct GG_State GG_State;
typedef struct ExitTrampolines ExitTrampolines;

typedef struct TGAlloc {
  void *reserved;
} TGAlloc;

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
  GCRef *ssb_next, *ssb_end, *ssb_base;
  SBuf tmpbuf;
  TValue tmptv, tmptv2;
  PRNGState prng;
  lua_State *thread_L;
  TGState *next_tg;
  uint64_t local_total;
  uint64_t stack_dirty_epoch;
  ExitTrampolines *exittr;
};

#define TG_DISP2HOT	(-(int)(HOTCOUNT_SIZE*sizeof(HotCount)))
#define TG_OFS(f) \
  ((int)(offsetof(TGState, f) - offsetof(TGState, dispatch)))
#define DISPATCH_TG(f)	TG_OFS(f)

LJ_FUNC void lj_tg_init(GG_State *GG);
LJ_FUNC void lj_tg_fini(global_State *g);

#endif
