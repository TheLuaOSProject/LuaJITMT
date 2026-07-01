/*
** Per-OS-thread state block scaffolding for LuaJIT-MT.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_TG_H
#define _LJ_TG_H

#include "lj_obj.h"
#include "lj_bc.h"
#include "lj_arena.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#endif

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
#define GG_LEN_SDISP	BC__MAX
#define GG_LEN_DISP	(GG_LEN_DDISP + GG_LEN_SDISP)

#define TGF_ARENA_INTERNAL	0x01u
#define TGF_HUGETAB		0x02u
#define TGF_DEAD		0x04u
#define TGF_STOPREQ		0x08u
#define TGF_STOPREQ_FRESH	0x10u
#define TG_HUGETAB_BITS		16u
#define TG_GC2_SSB_BYTES	8192u
#define TG_GC2_SSB_SLOTS	(TG_GC2_SSB_BYTES / sizeof(GCRef))
#define TG_GC2_SSB_DYNAMIC	0x01u

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
  int jit_exitcode;
  int32_t vmstate;
  uint32_t profile_samples;
  int32_t profile_vmstate;
  uint32_t in_native;
  StrTabHdr *strtab_active_hdr;
  uint32_t strtab_active_depth;
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
#if LJ_HASFFI
  void *ffi_call_func;
  CCallbackRuntime cb;
#endif
  lua_State *thread_L;
  GCudata *thread_ud;
  uint32_t tid;
  TGState *next_tg;
  uint64_t local_total;
  uint64_t stack_dirty_epoch;
  ExitTrampolines *exittr;
};

LJ_STATIC_ASSERT(sizeof(((GC2SSBNode *)0)->slot) == TG_GC2_SSB_BYTES);

static LJ_AINLINE int32_t lj_tg_vmstate_load_acq(TGState *tg)
{
  return (int32_t)la_load32_acq((uint32_t *)&tg->vmstate);
}

static LJ_AINLINE void lj_tg_vmstate_store_rel(TGState *tg, int32_t vmstate)
{
  la_store32_rel((uint32_t *)&tg->vmstate, (uint32_t)vmstate);
}

static LJ_AINLINE uint32_t lj_tg_in_native_acq(const TGState *tg)
{
  return la_load32_acq(&tg->in_native);  /* 05 section 5.4.3 native ack. */
}

static LJ_AINLINE void lj_tg_in_native_rel(TGState *tg, uint32_t in_native)
{
  la_store32_rel(&tg->in_native, in_native);  /* 05 section 5.4.3. */
}

static LJ_AINLINE void lj_tg_in_native_store_rlx(TGState *tg,
						 uint32_t in_native)
{
  la_store32_rlx(&tg->in_native, in_native);
}

static LJ_AINLINE StrTabHdr *lj_tg_strtab_active_hdr_acq(const TGState *tg)
{
  return (StrTabHdr *)la_loadptr_acq((void *const *)&tg->strtab_active_hdr);
}

static LJ_AINLINE void lj_tg_strtab_active_hdr_rel(TGState *tg,
						   StrTabHdr *hdr)
{
  la_storeptr_rel((void **)&tg->strtab_active_hdr, hdr);
}

static LJ_AINLINE uint32_t lj_tg_strtab_active_depth_acq(const TGState *tg)
{
  return la_load32_acq(&tg->strtab_active_depth);
}

static LJ_AINLINE uint32_t lj_tg_strtab_active_depth_xchg(TGState *tg,
							  uint32_t depth)
{
  return la_xchg32_acqrel(&tg->strtab_active_depth, depth);
}

static LJ_AINLINE uint32_t lj_tg_in_native_inc_rel(TGState *tg)
{
  uint32_t depth = lj_tg_in_native_acq(tg);
  if (depth != ~(uint32_t)0)
    depth++;
  lj_tg_in_native_rel(tg, depth);
  return depth;
}

static LJ_AINLINE uint32_t lj_tg_in_native_dec_rel(TGState *tg)
{
  uint32_t depth = lj_tg_in_native_acq(tg);
  if (depth != 0)
    depth--;
  lj_tg_in_native_rel(tg, depth);
  return depth;
}

static LJ_AINLINE uint8_t lj_tg_gc_assist_acq(const TGState *tg)
{
  return la_load8_acq(&tg->gc_assist);  /* 05 section 5.11 assist reentry. */
}

static LJ_AINLINE void lj_tg_gc_assist_store_rlx(TGState *tg,
						 uint8_t gc_assist)
{
  la_store8_rlx(&tg->gc_assist, gc_assist);
}

static LJ_AINLINE uint8_t lj_tg_hookmask_load(const TGState *tg)
{
  return la_load8_acq(&tg->hookmask_th);
}

static LJ_AINLINE uint8_t lj_tg_hookmask_update(TGState *tg, uint8_t clear,
						uint8_t set)
{
  uint8_t old = lj_tg_hookmask_load(tg);
  for (;;) {
    uint8_t next = (uint8_t)((old & (uint8_t)~clear) | set);
    if (la_cas8(&tg->hookmask_th, &old, next, LA_ACQ_REL, LA_ACQ))
      return next;
  }
}

static LJ_AINLINE int lj_tg_hookmask_set_if_clear(TGState *tg,
						  uint8_t blocked,
						  uint8_t set)
{
  uint8_t old = lj_tg_hookmask_load(tg);
  for (;;) {
    uint8_t next;
    if ((old & blocked))
      return 0;
    next = (uint8_t)(old | set);
    if (la_cas8(&tg->hookmask_th, &old, next, LA_ACQ_REL, LA_ACQ))
      return 1;
  }
}

static LJ_AINLINE uint32_t lj_tg_profile_samples_xchg(TGState *tg,
						      uint32_t samples)
{
  return la_xchg32_acqrel(&tg->profile_samples, samples);
}

static LJ_AINLINE void lj_tg_profile_samples_add(TGState *tg,
						 uint32_t samples)
{
  (void)la_add32_rlx(&tg->profile_samples, samples);
}

static LJ_AINLINE int32_t lj_tg_profile_vmstate_load_acq(TGState *tg)
{
  return (int32_t)la_load32_acq((uint32_t *)&tg->profile_vmstate);
}

static LJ_AINLINE void lj_tg_profile_vmstate_store_rel(TGState *tg,
						       int32_t vmstate)
{
  la_store32_rel((uint32_t *)&tg->profile_vmstate, (uint32_t)vmstate);
}

static LJ_AINLINE uint32_t lj_tg_mark_active_acq(const TGState *tg)
{
  return la_load32_acq(&tg->mark_active);  /* 05 section 5.5 barrier mirror. */
}

static LJ_AINLINE void lj_tg_mark_active_rel(TGState *tg,
					     uint32_t mark_active)
{
  la_store32_rel(&tg->mark_active, mark_active);  /* 05 section 5.5. */
}

static LJ_AINLINE uint8_t lj_tg_alloc_black_acq(const TGState *tg)
{
  return la_load8_acq(&tg->alloc.alloc_black);  /* 05 section 5.5 alloc color. */
}

static LJ_AINLINE void lj_tg_alloc_black_rel(TGState *tg,
					     uint8_t alloc_black)
{
  la_store8_rel(&tg->alloc.alloc_black, alloc_black);  /* 05 section 5.5. */
}

#if LJ_HASJIT
static LJ_AINLINE int lj_tg_jit_exitcode_acq(const TGState *tg)
{
  return (int)la_load32_acq((uint32_t *)&tg->jit_exitcode);
}

static LJ_AINLINE void lj_tg_jit_exitcode_rel(TGState *tg, int exitcode)
{
  la_store32_rel((uint32_t *)&tg->jit_exitcode, (uint32_t)exitcode);
}
#endif

#if LJ_HASFFI
static LJ_AINLINE void *lj_tg_ffi_call_func_acq(const TGState *tg)
{
  return la_loadptr_acq((void *const *)&tg->ffi_call_func);  /* 11.5 callback. */
}

static LJ_AINLINE void lj_tg_ffi_call_func_rel(TGState *tg, void *func)
{
  la_storeptr_rel((void **)&tg->ffi_call_func, func);  /* 11.5 callback. */
}
#endif

static LJ_AINLINE uint8_t lj_tg_flags_acq(const TGState *tg)
{
  return la_load8_acq(&tg->tg_flags);  /* 05 section 5.4.1 TG registry. */
}

static LJ_AINLINE void lj_tg_flags_store_rlx(TGState *tg, uint8_t flags)
{
  la_store8_rlx(&tg->tg_flags, flags);
}

static LJ_AINLINE uint8_t lj_tg_flags_or_rlx(TGState *tg, uint8_t flags)
{
  return la_or8_rlx(&tg->tg_flags, flags);  /* 05 section 5.4.1/09 section 9.6. */
}

static LJ_AINLINE uint8_t lj_tg_flags_and_rlx(TGState *tg, uint8_t flags)
{
  return la_and8_rlx(&tg->tg_flags, flags);
}

static LJ_AINLINE int lj_tg_flags_test_acq(const TGState *tg, uint8_t flags)
{
  return (lj_tg_flags_acq(tg) & flags) != 0;
}

static LJ_AINLINE int lj_tg_flags_all_acq(const TGState *tg, uint8_t flags)
{
  return (lj_tg_flags_acq(tg) & flags) == flags;
}

static LJ_AINLINE GC2SSBNode *lj_gc2_ssb_next_acq(const GC2SSBNode *node)
{
  return (GC2SSBNode *)la_loadptr_acq((void *const *)&node->next);
}

static LJ_AINLINE void lj_gc2_ssb_next_rel(GC2SSBNode *node,
					   GC2SSBNode *next)
{
  la_storeptr_rel((void **)&node->next, next);
}

static LJ_AINLINE TGState *lj_gc2_ssb_owner_acq(const GC2SSBNode *node)
{
  /* 05 section 5.6.2: SSB owner. */
  return (TGState *)la_loadptr_acq((void *const *)&node->owner);
}

static LJ_AINLINE void lj_gc2_ssb_owner_rel(GC2SSBNode *node, TGState *owner)
{
  /* 05 section 5.6.2: SSB owner. */
  la_storeptr_rel((void **)&node->owner, owner);
}

static LJ_AINLINE uint32_t lj_gc2_ssb_count_acq(const GC2SSBNode *node)
{
  /* 05 section 5.6.2: SSB item count. */
  return la_load32_acq(&node->n);
}

static LJ_AINLINE void lj_gc2_ssb_count_rel(GC2SSBNode *node, uint32_t n)
{
  /* 05 section 5.6.2: SSB item count. */
  la_store32_rel(&node->n, n);
}

static LJ_AINLINE GC2SSBNode *lj_tg_ssb_free_acq(const TGState *tg)
{
  return (GC2SSBNode *)la_loadptr_acq(
    (void *const *)&tg->ssb_free);  /* 05 section 5.6.2 SSB. */
}

static LJ_AINLINE void lj_tg_ssb_free_store_rlx(TGState *tg,
						GC2SSBNode *node)
{
  la_storeptr_rlx((void **)&tg->ssb_free, node);  /* 05 section 5.6.2 SSB. */
}

static LJ_AINLINE int lj_tg_ssb_free_cas(TGState *tg, GC2SSBNode **oldp,
					 GC2SSBNode *node)
{
  return la_casptr((void **)&tg->ssb_free, (void **)oldp, node,
		   LA_ACQ_REL, LA_ACQ);  /* 05 section 5.6.2 SSB free stack. */
}

static LJ_AINLINE GC2SSBNode *lj_tg_ssb_free_pop(TGState *tg)
{
  GC2SSBNode *head = lj_tg_ssb_free_acq(tg);
  /* The owner TG is the only popper; GC2 workers/assists only push recycle. */
  while (head != NULL) {
    GC2SSBNode *next = lj_gc2_ssb_next_acq(head);
    GC2SSBNode *oldhead = head;
    if (lj_tg_ssb_free_cas(tg, &oldhead, next)) {
      lj_gc2_ssb_next_rel(head, NULL);
      return head;
    }
    head = oldhead;
  }
  return NULL;
}

static LJ_AINLINE void lj_tg_ssb_free_push(TGState *tg, GC2SSBNode *node)
{
  GC2SSBNode *head = lj_tg_ssb_free_acq(tg);
  do {
    lj_gc2_ssb_next_rel(node, head);
  } while (!lj_tg_ssb_free_cas(tg, &head, node));
}

static LJ_AINLINE GC2SSBNode *lj_tg_ssb_active_acq(const TGState *tg)
{
  return (GC2SSBNode *)la_loadptr_acq(
    (void *const *)&tg->ssb_active);  /* 05 section 5.6.2 active SSB. */
}

static LJ_AINLINE void lj_tg_ssb_active_rel(TGState *tg, GC2SSBNode *node)
{
  la_storeptr_rel((void **)&tg->ssb_active, node);  /* 05 section 5.6.2. */
}

static LJ_AINLINE GCRef *lj_tg_ssb_base_acq(const TGState *tg)
{
  return (GCRef *)la_loadptr_acq(
    (void *const *)&tg->ssb_base);  /* 05 section 5.6.2 SSB cursor. */
}

static LJ_AINLINE void lj_tg_ssb_base_rel(TGState *tg, GCRef *base)
{
  la_storeptr_rel((void **)&tg->ssb_base, base);  /* 05 section 5.6.2. */
}

static LJ_AINLINE GCRef *lj_tg_ssb_next_acq(const TGState *tg)
{
  return (GCRef *)la_loadptr_acq(
    (void *const *)&tg->ssb_next);  /* 05 section 5.6.2 SSB cursor. */
}

static LJ_AINLINE void lj_tg_ssb_next_rel(TGState *tg, GCRef *next)
{
  la_storeptr_rel((void **)&tg->ssb_next, next);  /* 05 section 5.6.2. */
}

static LJ_AINLINE GCRef *lj_tg_ssb_end_acq(const TGState *tg)
{
  return (GCRef *)la_loadptr_acq(
    (void *const *)&tg->ssb_end);  /* 05 section 5.6.2 SSB cursor. */
}

static LJ_AINLINE void lj_tg_ssb_end_rel(TGState *tg, GCRef *end)
{
  la_storeptr_rel((void **)&tg->ssb_end, end);  /* 05 section 5.6.2. */
}

static LJ_AINLINE uint32_t lj_tg_tid_acq(const TGState *tg)
{
  return la_load32_acq(&tg->tid);  /* 05 section 5.4.1 TG owner id. */
}

static LJ_AINLINE void lj_tg_tid_rel(TGState *tg, uint32_t tid)
{
  lj_arena_alloc_owner_rel(&tg->alloc, tid);
  la_store32_rel(&tg->tid, tid);  /* Publish allocator owner before TG id. */
}

static LJ_AINLINE TGState *lj_tg_next_acq(const TGState *tg)
{
  return (TGState *)la_loadptr_acq((void *const *)&tg->next_tg);
}

static LJ_AINLINE void lj_tg_next_rel(TGState *tg, TGState *next)
{
  la_storeptr_rel((void **)&tg->next_tg, next);
}

static LJ_AINLINE uint32_t lj_tg_poll_acq(const TGState *tg)
{
  return la_load32_acq(&tg->poll);
}

static LJ_AINLINE void lj_tg_poll_store_rlx(TGState *tg, uint32_t poll)
{
  la_store32_rlx(&tg->poll, poll);
}

static LJ_AINLINE void lj_tg_poll_rel(TGState *tg, uint32_t poll)
{
  la_store32_rel(&tg->poll, poll);
}

static LJ_AINLINE void lj_tg_poll_futex_wait(TGState *tg, uint32_t poll,
					     int timeout_ns)
{
  la_futex_wait(&tg->poll, poll, timeout_ns);
}

static LJ_AINLINE void lj_tg_poll_futex_wake(TGState *tg, int n)
{
  la_futex_wake(&tg->poll, n);
}

static LJ_AINLINE uint32_t lj_tg_reqmask_acq(const TGState *tg)
{
  return la_load32_acq(&tg->reqmask);
}

static LJ_AINLINE void lj_tg_reqmask_store_rlx(TGState *tg, uint32_t reqmask)
{
  la_store32_rlx(&tg->reqmask, reqmask);
}

static LJ_AINLINE void lj_tg_reqmask_rel(TGState *tg, uint32_t reqmask)
{
  la_store32_rel(&tg->reqmask, reqmask);
}

static LJ_AINLINE uint32_t lj_tg_reqmask_xchg_acqrel(TGState *tg,
						     uint32_t reqmask)
{
  return la_xchg32_acqrel(&tg->reqmask, reqmask);
}

static LJ_AINLINE uint64_t lj_tg_hs_epoch_ack_acq(const TGState *tg)
{
  return la_load64_acq(&tg->hs_epoch_ack);
}

static LJ_AINLINE void lj_tg_hs_epoch_ack_store_rlx(TGState *tg,
						    uint64_t epoch)
{
  la_store64_rlx(&tg->hs_epoch_ack, epoch);
}

static LJ_AINLINE void lj_tg_hs_epoch_ack_rel(TGState *tg, uint64_t epoch)
{
  la_store64_rel(&tg->hs_epoch_ack, epoch);
}

static LJ_AINLINE int lj_tg_hs_epoch_ack_cas(TGState *tg, uint64_t *oldp,
					     uint64_t epoch)
{
  return la_cas64(&tg->hs_epoch_ack, oldp, epoch, LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE uint64_t lj_tg_local_total_xchg_acqrel(TGState *tg,
							 uint64_t bytes)
{
  return la_xchg64_acqrel(&tg->local_total, bytes);  /* 04 section 4.8. */
}

static LJ_AINLINE uint64_t lj_tg_local_total_add_rlx(TGState *tg,
						     uint64_t bytes)
{
  return la_add64_rlx(&tg->local_total, bytes);  /* 04 section 4.8. */
}

static LJ_AINLINE uint64_t lj_tg_stack_dirty_epoch_acq(const TGState *tg)
{
  return la_load64_acq(&tg->stack_dirty_epoch);  /* 05 section 5.7.3. */
}

static LJ_AINLINE uint64_t lj_tg_stack_dirty_epoch_add_rlx(TGState *tg,
							   uint64_t n)
{
  return la_add64_rlx(&tg->stack_dirty_epoch, n);  /* 05 section 5.7.3. */
}

static LJ_AINLINE lua_State *lj_tg_load_cur_L(TGState *tg)
{
  return (lua_State *)la_loadptr_acq((void *const *)&tg->cur_L);
}

static LJ_AINLINE void lj_tg_store_cur_L(TGState *tg, lua_State *L)
{
  la_storeptr_rel((void **)&tg->cur_L, L);  /* 05 section 5.7.4 TG root. */
}

static LJ_AINLINE lua_State *lj_tg_load_thread_L(TGState *tg)
{
  return (lua_State *)la_loadptr_acq((void *const *)&tg->thread_L);
}

static LJ_AINLINE void lj_tg_store_thread_L(TGState *tg, lua_State *L)
{
  la_storeptr_rel((void **)&tg->thread_L, L);  /* 05 section 5.7.4 TG root. */
}

static LJ_AINLINE TValue *lj_tg_load_jit_base(TGState *tg)
{
  return (TValue *)la_loadptr_acq((void *const *)&tg->jit_base);
}

static LJ_AINLINE void lj_tg_store_jit_base(TGState *tg, TValue *base)
{
  la_storeptr_rel((void **)&tg->jit_base, base);  /* 08 section 8.7 exit root. */
}

static LJ_AINLINE lua_State *lj_tg_cur_L(global_State *g)
{
  TGState *tg = G2TG(g);
  if (tg)
    return lj_tg_load_cur_L(tg);
  {
    GCobj *o = gcref_acq(g->cur_L);
    return o ? gco2th(o) : NULL;
  }
}

static LJ_AINLINE void lj_tg_setcur_L(global_State *g, lua_State *L)
{
  TGState *tg = G2TG(g);
  if (tg)
    lj_tg_store_cur_L(tg, L);
  setgcrefrel(g->cur_L, obj2gco(L));  /* Transitional mirror for VM asm. */
}

static LJ_AINLINE void lj_tg_clearcur_L(global_State *g)
{
  TGState *tg = G2TG(g);
  if (tg)
    lj_tg_store_cur_L(tg, NULL);
  setgcrefnullrel(g->cur_L);  /* Transitional mirror for VM asm. */
}

static LJ_AINLINE TValue *lj_tg_jit_base(global_State *g)
{
  TGState *tg = G2TG(g);
  if (tg)
    return lj_tg_load_jit_base(tg);
  return mref_acq(g->jit_base, TValue);  /* Transitional mirror for VM asm writes. */
}

static LJ_AINLINE void lj_tg_setjit_base(global_State *g, TValue *base)
{
  TGState *tg = G2TG(g);
  if (tg)
    lj_tg_store_jit_base(tg, base);
  setmrefrel(g->jit_base, base);  /* Transitional mirror for VM asm. */
}

#define TG_DISP2HOT	(-(int)(HOTCOUNT_SIZE*sizeof(HotCount)))
#define TG_OFS(f) \
  ((int)(offsetof(TGState, f) - offsetof(TGState, dispatch)))
#define DISPATCH_TG(f)	TG_OFS(f)

LJ_FUNC void lj_tg_init(GG_State *GG, int alloc_ready);
LJ_FUNC void lj_tg_fini(global_State *g);
LJ_FUNC void lj_tg_init_thread(global_State *g, TGState *tg, lua_State *L,
			       int arena_internal);
LJ_FUNC void lj_tg_derive_prng(global_State *g, TGState *tg, uint32_t tid);
LJ_FUNC void lj_tg_fini_ssb(TGState *tg);
LJ_FUNC void lj_tg_fini_thread(global_State *g, TGState *tg);
LJ_FUNC void lj_tg_attach(global_State *g, TGState *tg);
LJ_FUNC void lj_tg_detach(global_State *g, TGState *tg);
LJ_FUNC uint32_t lj_tg_reclaim_dead(global_State *g);
LJ_FUNC TGState *lj_tg_find_owner(global_State *g, uint32_t owner_tid);
LJ_FUNC void lj_tg_sync_dispatch_tg(global_State *g, TGState *tg);
LJ_FUNC void lj_tg_sync_dispatch(global_State *g);

#endif
