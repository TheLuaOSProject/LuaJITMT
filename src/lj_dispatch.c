/*
** Instruction dispatch handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_dispatch_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_func.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_meta.h"
#include "lj_debug.h"
#include "lj_state.h"
#include "lj_frame.h"
#include "lj_bc.h"
#include "lj_ff.h"
#include "lj_strfmt.h"
#include "lj_atomic.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_thr.h"
#if LJ_HASJIT
#include "lj_jit.h"
#endif
#if LJ_HASFFI
#include "lj_ccallback.h"
#endif
#include "lj_trace.h"
#include "lj_dispatch.h"
#if LJ_HASPROFILE
#include "lj_profile.h"
#endif
#include "lj_vm.h"
#include "luajit.h"

/* Bump GG_NUM_ASMFF in lj_dispatch.h as needed. Ugly. */
LJ_STATIC_ASSERT(GG_NUM_ASMFF == FF_NUM_ASMFUNC);

void LJ_FASTCALL lj_bc_publish_vm(uint32_t *pc, uint32_t ins)
{
  bc_publish(pc, ins);
}

void LJ_FASTCALL lj_bc_publish_op_vm(uint32_t *pc, BCOp op)
{
  bc_publish_op(pc, op);
}

#if defined(LJ_GC2_TEST_HELPERS) || defined(LJ_TRACE_TEST_HELPERS)
static uint32_t bc_test_publish_cas_collision_word;
static uint32_t bc_test_publish_cas_collision_armed;

void lj_bc_test_force_publish_cas_collision(uint32_t replacement)
{
  la_store32_rel(&bc_test_publish_cas_collision_word, replacement);
  la_store32_rel(&bc_test_publish_cas_collision_armed, 1);
}

uint32_t lj_bc_test_publish_cas_collision_pending(void)
{
  return la_load32_acq(&bc_test_publish_cas_collision_armed);
}

static void bc_test_publish_cas_collision(uint32_t *pc)
{
  uint32_t armed = 1;
  if (la_cas32(&bc_test_publish_cas_collision_armed, &armed, 0,
	       LA_ACQ_REL, LA_ACQ)) {
    uint32_t replacement =
      la_load32_acq(&bc_test_publish_cas_collision_word);
    bc_publish(pc, replacement);
  }
}
#else
#define bc_test_publish_cas_collision(pc) ((void)(pc))
#endif

uint32_t LJ_FASTCALL lj_bc_publish_cas_vm(uint32_t *pc, uint32_t expected,
					  uint32_t ins)
{
  bc_test_publish_cas_collision(pc);
  return bc_publish_cas(pc, &expected, ins) ? ins : expected;
}

/* -- Dispatch table management ------------------------------------------- */

#if LJ_TARGET_MIPS
#include <math.h>
LJ_FUNCA_NORET void LJ_FASTCALL lj_ffh_coroutine_wrap_err(lua_State *L,
							  lua_State *co);
#if !LJ_HASJIT
#define lj_dispatch_stitch	lj_dispatch_ins
#endif
#if !LJ_HASPROFILE
#define lj_dispatch_profile	lj_dispatch_ins
#endif

#define GOTFUNC(name)	(ASMFunction)name,
static const ASMFunction dispatch_got[] = {
  GOTDEF(GOTFUNC)
};
#undef GOTFUNC
#endif

/* Initialize instruction dispatch table and hot counters. */
void lj_dispatch_init(GG_State *GG)
{
  uint32_t i;
  ASMFunction *disp = GG->dispatch;
  for (i = 0; i < GG_LEN_SDISP; i++)
    disp[GG_LEN_DDISP+i] = disp[i] = makeasmfunc(lj_bc_ofs[i]);
  for (i = GG_LEN_SDISP; i < GG_LEN_DDISP; i++)
    disp[i] = makeasmfunc(lj_bc_ofs[i]);
  /* The JIT engine is off by default. luaopen_jit() turns it on. */
  disp[BC_FORL] = disp[BC_IFORL];
  disp[BC_ITERL] = disp[BC_IITERL];
  /* Workaround for stable v2.1 bytecode. TODO: Replace with BC_IITERN. */
  disp[BC_ITERN] = &lj_vm_IITERN;
  disp[BC_LOOP] = disp[BC_ILOOP];
  disp[BC_FUNCF] = disp[BC_IFUNCF];
  disp[BC_FUNCV] = disp[BC_IFUNCV];
  GG->g.bc_cfunc_ext = GG->g.bc_cfunc_int = BCINS_AD(BC_FUNCC, LUA_MINSTACK, 0);
  for (i = 0; i < GG_NUM_ASMFF; i++)
    GG->bcff[i] = BCINS_AD(BC__MAX+i, 0, 0);
#if LJ_TARGET_MIPS
  memcpy(GG->got, dispatch_got, LJ_GOT__MAX*sizeof(ASMFunction *));
#endif
}

#if LJ_HASJIT
/* The low 16 bits are the exact HotCount bucket template. The remaining high
** bits are a non-wrapping generation, published with the template by one
** 64-bit CAS so attach/ACK readers cannot accept a torn pair. */
#define HOTCOUNT_RESET_GENERATION_SHIFT 16u
#define HOTCOUNT_RESET_VALUE_MASK UINT64_C(0xffff)
#define HOTCOUNT_RESET_GENERATION_MAX \
  (UINT64_MAX >> HOTCOUNT_RESET_GENERATION_SHIFT)

static LJ_AINLINE uint64_t hotcount_reset_generation(uint64_t word)
{
  return word >> HOTCOUNT_RESET_GENERATION_SHIFT;
}

static LJ_AINLINE HotCount hotcount_reset_value(uint64_t word)
{
  return (HotCount)(word & HOTCOUNT_RESET_VALUE_MASK);
}

uint64_t lj_dispatch_hotcount_publish(global_State *g)
{
  TGState *main_tg = g ? g->main_tg : NULL;
  uint32_t hotloop;
  HotCount start;
  uint64_t oldword;
  if (!main_tg || !G2J(g))
    return 0;
  hotloop = (uint32_t)jit_param_acq(G2J(g), JIT_P_hotloop);
  start = (HotCount)(hotloop * (uint32_t)HOTCOUNT_LOOP - 1u);
  oldword = lj_tg_hotcount_reset_word_acq(main_tg);
  for (;;) {
    uint64_t generation = hotcount_reset_generation(oldword);
    uint64_t desired;
    if (LJ_UNLIKELY(generation == HOTCOUNT_RESET_GENERATION_MAX))
      abort();  /* A wrapped generation could falsely satisfy a stale TG. */
    generation++;
    desired = (generation << HOTCOUNT_RESET_GENERATION_SHIFT) |
      (uint64_t)start;
    if (lj_tg_hotcount_reset_word_cas(main_tg, &oldword, desired)) {
      /* Dekker half for generation publication versus TG list attachment.
      ** The attaching owner fences after its successful list CAS and then
      ** rechecks this word. Thus either that recheck consumes this generation
      ** or the following handshake list scan consumes the new TG. */
      la_fence_seq();
      return generation;
    }
  }
}

uint64_t lj_dispatch_hotcount_apply_tg(global_State *g, TGState *tg)
{
  TGState *main_tg = g ? g->main_tg : NULL;
  uint64_t word, generation;
  uint32_t i;
  if (!main_tg || !tg || tg->gl != g)
    return 0;
  for (;;) {
    HotCount start;
    word = lj_tg_hotcount_reset_word_acq(main_tg);
    generation = hotcount_reset_generation(word);
    if (lj_tg_hotcount_applied_generation_acq(tg) == generation)
      return generation;
    start = hotcount_reset_value(word);
    for (i = 0; i < HOTCOUNT_SIZE; i++)
      tg->hotcount[i] = start;
    /* A newer packed publication which raced the plain owner fill requires a
    ** complete refill. Only the exact word sampled above may authorize the
    ** applied-generation release publication. */
    if (lj_tg_hotcount_reset_word_acq(main_tg) == word) {
      lj_tg_hotcount_applied_generation_rel(tg, generation);
      return generation;
    }
  }
}

/* Publish a new desired template, then make its completion a normal counted
** safepoint boundary over the mandatory legacy TG list. */
void lj_dispatch_init_hotcount(global_State *g)
{
  if (lj_dispatch_hotcount_publish(g) != 0)
    (void)lj_gc2_handshake(g, LJ_GC2_HS_RESET_HOTCOUNT);
}
#endif

/* Internal dispatch mode bits. */
#define DISPMODE_CALL	0x01	/* Override call dispatch. */
#define DISPMODE_RET	0x02	/* Override return dispatch. */
#define DISPMODE_INS	0x04	/* Override instruction dispatch. */
#define DISPMODE_JIT	0x10	/* JIT compiler on. */
#define DISPMODE_PROF	0x40	/* Profiling active. */
#define DISPMODE_UPDATE	0x80	/* Dispatch table update in progress. */

static void dispatch_setins_cells(ASMFunction *disp, ASMFunction f)
{
  uint32_t i;
  for (i = BC_CNEW; i <= BC_CSET; i++)
    disp[i] = f;
}

static void dispatch_copyins_cells(ASMFunction *disp)
{
  uint32_t i;
  for (i = BC_CNEW; i <= BC_CSET; i++)
    disp[i] = disp[GG_LEN_DDISP+i];
}

static void dispatch_setcall(ASMFunction *disp, ASMFunction f)
{
  uint32_t i;
  for (i = BC_FUNCF; i <= BC_FUNCCW; i++)
    disp[i] = f;
  for (i = BC__MAX; i < GG_LEN_DDISP; i++)
    disp[i] = f;
}

static void dispatch_copycall(ASMFunction *disp)
{
  uint32_t i;
  for (i = BC_FUNCF; i <= BC_FUNCCW; i++)
    disp[i] = makeasmfunc(lj_bc_ofs[i]);
  for (i = BC__MAX; i < GG_LEN_DDISP; i++)
    disp[i] = makeasmfunc(lj_bc_ofs[i]);
}

#if LJ_HASJIT
static void dispatch_setrecord(ASMFunction *disp, uint8_t mode)
{
  ASMFunction f = (mode & DISPMODE_PROF) ? lj_vm_profhook : lj_vm_record;
  uint32_t i;
  disp[GG_LEN_DDISP+BC_FORL] = disp[GG_LEN_DDISP+BC_IFORL];
  disp[GG_LEN_DDISP+BC_ITERL] = disp[GG_LEN_DDISP+BC_IITERL];
  disp[GG_LEN_DDISP+BC_ITERN] = &lj_vm_IITERN;
  disp[GG_LEN_DDISP+BC_LOOP] = disp[GG_LEN_DDISP+BC_ILOOP];
  for (i = 0; i < BC_FUNCF; i++)
    disp[i] = f;
  dispatch_setins_cells(disp, f);
  dispatch_setcall(disp, lj_vm_callhook);
}

static uint8_t dispatch_record_mode(global_State *g, TGState *tg)
{
  uint8_t mode = dispatchmode_load_acq(g);
#if LJ_PROFILE_TGLOCAL
  if (lj_tg_hookmask_load(tg) & HOOK_PROFILE)
    mode |= DISPMODE_PROF;
#else
  UNUSED(tg);
#endif
  return mode;
}

/* Revalidate the complete published recorder-owner tuple. The token contains
** only the process thread id, so the state/TG actor certificate and current-L
** publication are required as well. An asynchronously aborted recorder still
** owns this tuple until trace_state() publishes IDLE and releases the token;
** it needs one final recording dispatch in order to consume that abort. */
static int dispatch_recorder_owner(global_State *g, TGState *tg,
				   lua_State *L, jit_State *J)
{
  uint32_t tid;
  if (!g || !tg || !L || !J || J != G2J(g) || tg->gl != g)
    return 0;
  tid = lj_tg_tid_acq(tg);
  if (tid == 0 || jit_token_acq(g) != tid || jit_owner_l_acq(J) != L ||
      lj_trace_state_load(J) == LJ_TRACE_IDLE ||
      lj_tg_load_cur_L(tg) != L)
    return 0;
  /* The target TG's current-L publication is the lifetime authority needed
  ** before dereferencing the sampled state. */
  return lj_tg_owns_state_acq(tg, L) && G(L) == g;
}

/* Start recording by modifying only the current owner's private dispatch
** table. trace_state() is executing outside VM dispatch, so no bytecode can
** concurrently consume a partially installed owner-local overlay. The
** global template and DISPMODE_UPDATE claim are deliberately untouched. */
int lj_dispatch_record_start(lua_State *L, jit_State *J)
{
  global_State *g = J ? J2G(J) : NULL;
  TGState *tg = L ? L2TG(L) : NULL;
  uint8_t mode;
  if (!dispatch_recorder_owner(g, tg, L, J))
    return 0;
  mode = dispatch_record_mode(g, tg);
  dispatch_setrecord(tg->dispatch, mode);
  return dispatch_recorder_owner(g, tg, L, J);
}
#endif

/* A REDISPATCH acknowledgement must not replace an active or asynchronously
** aborted recorder's overlay with the ordinary template. Only the stopped TG
** writes its own table. The global template-generation race in the existing
** update/handshake protocol is separate from this recorder preservation and
** remains visible until dispatch templates become immutable generations. */
void lj_dispatch_sync_tg(global_State *g, TGState *tg)
{
#if LJ_HASJIT
  jit_State *J;
  lua_State *L;
#endif
  lj_tg_sync_dispatch_tg(g, tg);
#if LJ_HASJIT
  if (!g || !tg)
    return;
  J = G2J(g);
  L = lj_tg_load_cur_L(tg);
  if (dispatch_recorder_owner(g, tg, L, J))
    dispatch_setrecord(tg->dispatch, dispatch_record_mode(g, tg));
#endif
}

static uint8_t dispatch_state_mode(global_State *g)
{
  uint8_t mode = 0;
  uint8_t hookmask = hookmask_load(g);
#if LJ_HASJIT
  jit_State *J = G2J(g);
  mode |= (jit_flags_acq(J) & JIT_F_ON) ? DISPMODE_JIT : 0;
#endif
#if LJ_HASPROFILE
#if !LJ_PROFILE_TGLOCAL
  mode |= (hookmask & HOOK_PROFILE) ? (DISPMODE_PROF|DISPMODE_INS) : 0;
#endif
#endif
  mode |= (hookmask & (LUA_MASKLINE|LUA_MASKCOUNT)) ? DISPMODE_INS : 0;
  mode |= (hookmask & LUA_MASKCALL) ? DISPMODE_CALL : 0;
  mode |= (hookmask & LUA_MASKRET) ? DISPMODE_RET : 0;
  return mode;
}

static void dispatch_update_wait_no_l(void)
{
  (void)lj_thr_retry_yield(NULL);
}

/* Update dispatch table depending on various flags. */
void LJ_FASTCALL lj_dispatch_update(global_State *g, int nolock)
{
  uint32_t redispatch = 0;
#if LJ_HASJIT
  uint32_t reset_hotcount = 0;
#endif
  uint8_t oldmode, mode;
retry:
  oldmode = dispatchmode_load_acq(g);
  if ((oldmode & DISPMODE_UPDATE)) {
    /* Async profile triggers may interrupt the owner of the update token. */
    if (nolock > 1) return;
    dispatch_update_wait_no_l();
    goto retry;
  }
  mode = dispatch_state_mode(g);
  if (oldmode != mode) {  /* Mode changed? */
    uint8_t claim = (uint8_t)(oldmode | DISPMODE_UPDATE);
    ASMFunction *disp;
    ASMFunction f_forl, f_iterl, f_itern, f_loop, f_funcf, f_funcv;
#if LJ_HASJIT
    /* An async profile interruption must never become the publisher of an
    ** unrelated JIT off->on transition: it cannot run the counted reset
    ** handshake. The ordinary JIT controller will observe and publish it. */
    if (nolock > 1 && ((oldmode ^ mode) & DISPMODE_JIT))
      return;
#endif
    if (!dispatchmode_cas(g, &oldmode, claim))
      goto retry;
    disp = G2GG(g)->dispatch;

    /* Hotcount if JIT is on. Recording overlays are TG-local below. */
    if ((mode & DISPMODE_JIT)) {
      f_forl = makeasmfunc(lj_bc_ofs[BC_FORL]);
      f_iterl = makeasmfunc(lj_bc_ofs[BC_ITERL]);
      f_itern = makeasmfunc(lj_bc_ofs[BC_ITERN]);
      f_loop = makeasmfunc(lj_bc_ofs[BC_LOOP]);
      f_funcf = makeasmfunc(lj_bc_ofs[BC_FUNCF]);
      f_funcv = makeasmfunc(lj_bc_ofs[BC_FUNCV]);
    } else {  /* Otherwise use the non-hotcounting instructions. */
      f_forl = disp[GG_LEN_DDISP+BC_IFORL];
      f_iterl = disp[GG_LEN_DDISP+BC_IITERL];
      f_itern = &lj_vm_IITERN;
      f_loop = disp[GG_LEN_DDISP+BC_ILOOP];
      f_funcf = makeasmfunc(lj_bc_ofs[BC_IFUNCF]);
      f_funcv = makeasmfunc(lj_bc_ofs[BC_IFUNCV]);
    }
    /* Init static counting instruction dispatch first (may be copied below). */
    disp[GG_LEN_DDISP+BC_FORL] = f_forl;
    disp[GG_LEN_DDISP+BC_ITERL] = f_iterl;
    disp[GG_LEN_DDISP+BC_ITERN] = f_itern;
    disp[GG_LEN_DDISP+BC_LOOP] = f_loop;

    /* Set dynamic instruction dispatch. */
    if ((oldmode ^ mode) & (DISPMODE_PROF|DISPMODE_INS)) {
      /* Need to update the whole table. */
      if (!(mode & DISPMODE_INS)) {  /* No ins dispatch? */
	/* Copy static dispatch table to dynamic dispatch table. */
	memcpy(&disp[0], &disp[GG_LEN_DDISP], BC_FUNCF*sizeof(ASMFunction));
	dispatch_copyins_cells(disp);
	/* Overwrite with dynamic return dispatch. */
	if ((mode & DISPMODE_RET)) {
	  disp[BC_RETM] = lj_vm_rethook;
	  disp[BC_RET] = lj_vm_rethook;
	  disp[BC_RET0] = lj_vm_rethook;
	  disp[BC_RET1] = lj_vm_rethook;
	}
      } else {
	ASMFunction f = (mode & DISPMODE_PROF) ? lj_vm_profhook : lj_vm_inshook;
	uint32_t i;
	for (i = 0; i < BC_FUNCF; i++)
	  disp[i] = f;
	dispatch_setins_cells(disp, f);
      }
    } else if (!(mode & DISPMODE_INS)) {
      /* Otherwise set dynamic counting ins. */
      disp[BC_FORL] = f_forl;
      disp[BC_ITERL] = f_iterl;
      disp[BC_ITERN] = f_itern;
      disp[BC_LOOP] = f_loop;
      /* Set dynamic return dispatch. */
      if ((mode & DISPMODE_RET)) {
	disp[BC_RETM] = lj_vm_rethook;
	disp[BC_RET] = lj_vm_rethook;
	disp[BC_RET0] = lj_vm_rethook;
	disp[BC_RET1] = lj_vm_rethook;
      } else {
	disp[BC_RETM] = disp[GG_LEN_DDISP+BC_RETM];
	disp[BC_RET] = disp[GG_LEN_DDISP+BC_RET];
	disp[BC_RET0] = disp[GG_LEN_DDISP+BC_RET0];
	disp[BC_RET1] = disp[GG_LEN_DDISP+BC_RET1];
      }
    }

    /* Set dynamic call dispatch. */
    if ((oldmode ^ mode) & DISPMODE_CALL) {  /* Update the whole table? */
      if ((mode & DISPMODE_CALL) == 0) {  /* No call hooks? */
	dispatch_copycall(disp);
      } else {
	dispatch_setcall(disp, lj_vm_callhook);
      }
    }
    if (!(mode & DISPMODE_CALL)) {  /* Overwrite dynamic counting ins. */
      disp[BC_FUNCF] = f_funcf;
      disp[BC_FUNCV] = f_funcv;
    }

#if LJ_HASJIT
	/* Defer off->on reset publication until DISPMODE_UPDATE is released. */
	if ((mode & DISPMODE_JIT) && !(oldmode & DISPMODE_JIT))
	  reset_hotcount = 1;
#endif
	if (gc2_n_threads_acq(g) > 1)
	  redispatch = 1;
    dispatchmode_store_rel(g, mode);
    if (dispatch_state_mode(g) != mode)
      goto retry;
  }
#if LJ_HASJIT
  if (reset_hotcount && !(mode & DISPMODE_JIT))
    reset_hotcount = 0;
  if (reset_hotcount && lj_dispatch_hotcount_publish(g) == 0)
    abort();
#endif
  /* Never write a live peer's dispatch table here. Refresh only the caller's
  ** TG and perform a fresh exact-owner revalidation before any local recorder
  ** overlay. A foreign recorder is repaired by its REDISPATCH ACK. */
  lj_dispatch_sync_tg(g, G2TG(g));
  if (redispatch && nolock > 1)
    return;  /* Async profile triggers must not run an MT handshake. */
#if LJ_HASJIT
  if (reset_hotcount) {
    /* Publish mode and reset generation before the one combined counted
    ** boundary. REDISPATCH installs hot opcodes on each stopped TG and the
    ** reset action fills that same TG before its pending slot is released. */
    (void)lj_gc2_handshake(g,
	LJ_GC2_HS_REDISPATCH|LJ_GC2_HS_RESET_HOTCOUNT);
    return;
  }
#endif
  if (redispatch)
    (void)lj_gc2_handshake(g, LJ_GC2_HS_REDISPATCH);
}

/* -- JIT mode setting ---------------------------------------------------- */

#if LJ_HASJIT
/* Set JIT mode for a single prototype. */
static uint32_t setptmode(global_State *g, GCproto *pt, int mode)
{
  if ((mode & LUAJIT_MODE_ON)) {  /* (Re-)enable JIT compilation. */
    pt->flags &= ~PROTO_NOJIT;
    lj_trace_reenableproto(pt);  /* Unpatch all ILOOP etc. bytecodes. */
    return 0;
  } else {  /* Flush and/or disable JIT compilation. */
    uint32_t flushed;
    if (!(mode & LUAJIT_MODE_FLUSH))
      pt->flags |= PROTO_NOJIT;
    flushed = lj_trace_flushproto(g, pt);  /* Flush all traces of prototype. */
    return (mode & LUAJIT_MODE_FLUSH) ? flushed : flushed + 1u;
  }
}

/* Recursively set the JIT mode for all children of a prototype. */
static uint32_t setptmode_all(global_State *g, GCproto *pt, int mode)
{
  uint32_t flushed = 0;
  ptrdiff_t i;
  if (!(pt->flags & PROTO_CHILD)) return 0;
  for (i = -(ptrdiff_t)proto_sizekgc_acq(pt); i < 0; i++) {
    GCobj *o = proto_kgc_acq(pt, i);
    if (o->gch.gct == ~LJ_TPROTO) {
      flushed += setptmode(g, gco2pt(o), mode);
      flushed += setptmode_all(g, gco2pt(o), mode);
    }
  }
  return flushed;
}
#endif

static lua_State *setmode_errstate(lua_State *L)
{
  lua_State *cur = lj_tg_cur_L(G(L));
  return cur && G(cur) == G(L) ? cur : L;
}

static void setmode_checkclaim(lua_State *L, LJStateClaim *claim)
{
  if (!lj_state_tryclaim(L, lj_thr_current_id(G(L)), claim))
    lj_err_callermsg(setmode_errstate(L), "thread busy");
}

static cTValue *setmode_stack_slot(lua_State *L, int idx)
{
  if (idx > 0) {
    cTValue *o = L->base + (idx - 1);
    return o < L->top ? o : niltv(L);
  } else if (idx > LUA_REGISTRYINDEX && idx != 0 &&
	     -idx <= L->top - L->base) {
    return L->top + idx;
  }
  return niltv(L);
}

/* Public API function: control the JIT engine. */
int luaJIT_setmode(lua_State *L, int idx, int mode)
{
  global_State *g = G(L);
  int mm = mode & LUAJIT_MODE_MASK;
  lj_trace_abort(g);  /* Abort recording on any state change. */
  /* Avoid pulling the rug from under our own feet. */
  if ((hookmask_load(g) & HOOK_GC) &&
      lj_gc2_finalizer_owned_by_current(g))
    lj_err_caller(L, LJ_ERR_NOGCMM);
  switch (mm) {
#if LJ_HASJIT
  case LUAJIT_MODE_ENGINE:
    if ((mode & LUAJIT_MODE_FLUSH)) {
      (void)lj_trace_flushall_hs(L);
    } else {
      int token = lj_jit_token_acquire_wait(G2J(g));
      if (!(mode & LUAJIT_MODE_ON))
	jit_flags_setmask(G2J(g), JIT_F_ON, 0);
      else
	jit_flags_setmask(G2J(g), 0, JIT_F_ON);
      lj_dispatch_update(g, 0);
      if (token)
	lj_jit_token_release(G2J(g));
    }
    break;
  case LUAJIT_MODE_FUNC:
  case LUAJIT_MODE_ALLFUNC:
  case LUAJIT_MODE_ALLSUBFUNC: {
    LJStateClaim claim;
    uint32_t flushed = 0;
    int token = lj_jit_token_acquire_wait(G2J(g));
    cTValue *tv;
    GCproto *pt;
    if (!lj_state_tryclaim(L, lj_thr_current_id(g), &claim)) {
      if (token)
	lj_jit_token_release(G2J(g));
      lj_err_callermsg(setmode_errstate(L), "thread busy");
    }
    tv = idx == 0 ? frame_prev(L->base-1)-LJ_FR2 :
		    setmode_stack_slot(L, idx);
    if ((idx == 0 || tvisfunc(tv)) && isluafunc(&gcval(tv)->fn))
      pt = funcproto(&gcval(tv)->fn);  /* Cannot use funcV() for frame slot. */
    else if (tvisproto(tv))
      pt = protoV(tv);
    else {
      lj_state_dropclaim(&claim);
      if (token)
	lj_jit_token_release(G2J(g));
      return 0;  /* Failed. */
    }
    if (mm != LUAJIT_MODE_ALLSUBFUNC)
      flushed += setptmode(g, pt, mode);
    if (mm != LUAJIT_MODE_FUNC)
      flushed += setptmode_all(g, pt, mode);
    lj_state_dropclaim(&claim);
    if (!(mode & LUAJIT_MODE_ON))
      lj_trace_flushscope_hs(g, flushed);
    if (token)
      lj_jit_token_release(G2J(g));
    break;
    }
  case LUAJIT_MODE_TRACE:
    if (!(mode & LUAJIT_MODE_FLUSH))
      return 0;  /* Failed. */
    (void)lj_trace_flushscope(G2J(g), idx);
    break;
#else
  case LUAJIT_MODE_ENGINE:
  case LUAJIT_MODE_FUNC:
  case LUAJIT_MODE_ALLFUNC:
  case LUAJIT_MODE_ALLSUBFUNC:
    UNUSED(idx);
    if ((mode & LUAJIT_MODE_ON))
      return 0;  /* Failed. */
    break;
#endif
  case LUAJIT_MODE_WRAPCFUNC:
    if ((mode & LUAJIT_MODE_ON)) {
      if (idx != 0) {
	LJStateClaim claim;
	cTValue *tv;
	setmode_checkclaim(L, &claim);
	tv = setmode_stack_slot(L, idx);
	if (tvislightud(tv)) {
	  wrapf_store(g, (lua_CFunction)lightudV(g, tv));
	  lj_state_dropclaim(&claim);
	} else {
	  lj_state_dropclaim(&claim);
	  return 0;  /* Failed. */
	}
      } else {
	return 0;  /* Failed. */
      }
      bc_publish_op(&g->bc_cfunc_ext, BC_FUNCCW);
    } else {
      bc_publish_op(&g->bc_cfunc_ext, BC_FUNCC);
    }
    break;
  default:
    return 0;  /* Failed. */
  }
  return 1;  /* OK. */
}

/* Enforce (dynamic) linker error for version mismatches. See luajit.c. */
LUA_API void LUAJIT_VERSION_SYM(void)
{
}

/* -- Hooks --------------------------------------------------------------- */

/* This function can be called asynchronously (e.g. during a signal). */
LUA_API int lua_sethook(lua_State *L, lua_Hook func, int mask, int count)
{
  global_State *g = G(L);
  mask &= HOOK_EVENTMASK;
  if (func == NULL || mask == 0) { mask = 0; func = NULL; }  /* Consistency. */
  hookf_store(g, func);
  hookcount_setstart(g, (int32_t)count);
  hookmask_setevents(g, (uint8_t)mask);
  lj_trace_abort(g);  /* Abort recording on any hook change. */
  lj_dispatch_update(g, 0);
  return 1;
}

LUA_API lua_Hook lua_gethook(lua_State *L)
{
  return hookf_load(G(L));
}

LUA_API int lua_gethookmask(lua_State *L)
{
  return hookmask_load(G(L)) & HOOK_EVENTMASK;
}

LUA_API int lua_gethookcount(lua_State *L)
{
  return (int)hookcstart_load(G(L));
}

/* Call a hook. */
static void callhook(lua_State *L, int event, BCLine line)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  lua_Hook hookf = hookf_load(g);
  if (hookf &&
      !(lj_tg_hookmask_combined_load(g, tg) & HOOK_ACTIVE)) {
    lua_Debug ar;
    lj_trace_abort(g);  /* Abort recording on any hook call. */
    ar.event = event;
    ar.currentline = line;
    /* Top frame, nextframe = NULL. */
    ar.i_ci = (int)((L->base-1) - tvref(L->stack));
    lj_state_checkstack(L, 1+LUA_MINSTACK);
#if LJ_HASPROFILE && !LJ_PROFILE_SIGPROF
    lj_profile_hook_enter(g);
#else
    hook_call_enter(g);
#endif
    hookf(L, &ar);
    lj_assertG(hook_active(g), "active hook flag removed");
    lj_tg_setcur_L(g, L);
#if LJ_HASPROFILE && !LJ_PROFILE_SIGPROF
    lj_profile_hook_leave(g);
#else
    hook_call_leave(g);
#endif
  }
}

/* -- Dispatch callbacks -------------------------------------------------- */

/* Calculate number of used stack slots in the current frame. */
static BCReg cur_topslot(GCproto *pt, const BCIns *pc, uint32_t nres)
{
  BCIns ins = pc[-1];
  if (bc_op(ins) == BC_UCLO)
    ins = pc[bc_j(ins)];
  switch (bc_op(ins)) {
  case BC_CALLM: case BC_CALLMT: return bc_a(ins) + bc_c(ins) + nres-1+1+LJ_FR2;
  case BC_RETM: return bc_a(ins) + bc_d(ins) + nres-1;
  case BC_TSETM: return bc_a(ins) + nres-1;
  default: return pt->framesize;
  }
}

/* Instruction dispatch. Used by instr/line/return hooks or when recording. */
#if LJ_TARGET_ARM64
int32_t LJ_FASTCALL lj_dispatch_hookcount_dec(global_State *g)
{
  /* Global hook count is shared by all TG dispatch overlays. Match the x64
  ** locked decrement and return the post-decrement value without a signed
  ** overflow in the C expression. */
  return (int32_t)(la_sub32_acqrel((uint32_t *)&g->hookcount, 1) - 1u);
}
#endif

void LJ_FASTCALL lj_dispatch_ins(lua_State *L, const BCIns *pc)
{
  ERRNO_SAVE
  GCfunc *fn = curr_func(L);
  GCproto *pt = funcproto(fn);
  void *cf = cframe_raw(L->cframe);
  const BCIns *oldpc = cframe_pc(cf);
  global_State *g = G(L);
  uint8_t hookmask;
  BCReg slots;
  setcframe_pc(cf, pc);
  slots = cur_topslot(pt, pc, cframe_multres_n(cf));
  L->top = L->base + slots;  /* Fix top. */
#if LJ_HASJIT
  {
    jit_State *J = G2J(g);
    if (lj_trace_state_load(J) != LJ_TRACE_IDLE &&
	jit_owner_l_acq(J) == L && lj_jit_token_held_l(L, J) &&
	lj_trace_state_load(J) != LJ_TRACE_IDLE &&
	!(lj_tg_hookmask_combined_load(g, L2TG(L)) &
	  (HOOK_GC|HOOK_VMEVENT))) {
#ifdef LUA_USE_ASSERT
      ptrdiff_t delta = L->top - L->base;
#endif
      lj_trace_ins(J, pc-1);  /* The interpreter bytecode PC is offset by 1. */
      if (lj_trace_state_load(J) == LJ_TRACE_IDLE ||
	  !lj_jit_token_held_l(L, J))
	lj_safepoint_checkstop(L, 0);
      lj_assertG(L->top - L->base == delta,
		 "unbalanced stack after tracing of instruction");
    }
  }
#endif
  hookmask = lj_tg_hookmask_combined_load(g, L2TG(L));
  if ((hookmask & LUA_MASKCOUNT) && hookcount_load(g) <= 0) {
    hookcount_reset(g);
    callhook(L, LUA_HOOKCOUNT, -1);
    L->top = L->base + slots;  /* Fix top again. */
  }
  hookmask = lj_tg_hookmask_combined_load(g, L2TG(L));
  if ((hookmask & LUA_MASKLINE)) {
    BCPos npc = proto_bcpos(pt, pc) - 1;
    BCPos opc = proto_bcpos(pt, oldpc) - 1;
    BCLine line = lj_debug_line(pt, npc);
    if (pc <= oldpc || opc >= pt->sizebc || line != lj_debug_line(pt, opc)) {
      callhook(L, LUA_HOOKLINE, line);
      L->top = L->base + slots;  /* Fix top again. */
    }
  }
  hookmask = lj_tg_hookmask_combined_load(g, L2TG(L));
  if ((hookmask & LUA_MASKRET) && bc_isret(bc_op(pc[-1])))
    callhook(L, LUA_HOOKRET, -1);
  ERRNO_RESTORE
}

/* Initialize call. Ensure stack space and return # of missing parameters. */
static int call_init(lua_State *L, GCfunc *fn)
{
  if (isluafunc(fn)) {
    GCproto *pt = funcproto(fn);
    int numparams = pt->numparams;
    int gotparams = (int)(L->top - L->base);
    int need = pt->framesize;
    if ((pt->flags & PROTO_VARARG)) need += 1+LJ_FR2+gotparams;
    lj_state_checkstack(L, (MSize)need);
    numparams -= gotparams;
    return numparams >= 0 ? numparams : 0;
  } else {
    lj_state_checkstack(L, LUA_MINSTACK);
    return 0;
  }
}

static void call_fill_missing(lua_State *L, int *missing)
{
  while (*missing > 0) {
    setnilV(L->top++);
    (*missing)--;
  }
  *missing = 0;
}

#if LJ_HASJIT
static void call_hot_poll(lua_State *L, int *missing)
{
  TGState *tg = L2TG(L);
  if (tg && lj_tg_poll_acq(tg) != 0) {
    call_fill_missing(L, missing);
    lj_safepoint_ack_check(L);
  }
}
#endif

/* Call dispatch. Used by call hooks, hot calls or when recording. */
ASMFunction LJ_FASTCALL lj_dispatch_call(lua_State *L, const BCIns *pc)
{
  ERRNO_SAVE
  GCfunc *fn = curr_func(L);
  BCOp op;
  global_State *g = G(L);
#if LJ_HASJIT
  jit_State *J = G2J(g);
#endif
  uint8_t hookmask;
  int missing = call_init(L, fn);
#if LJ_HASJIT
  if ((uintptr_t)pc & 1) {  /* Marker for hot call. */
    call_hot_poll(L, &missing);
#ifdef LUA_USE_ASSERT
    ptrdiff_t delta = L->top - L->base;
#endif
    pc = (const BCIns *)((uintptr_t)pc & ~(uintptr_t)1);
    op = bc_op(pc[-1]);
    if (op != BC_FUNCF && op != BC_FUNCV)
      goto out;  /* Another thread patched this function header first. */
#if LJ_TARGET_X64 || LJ_TARGET_ARM64
    lj_trace_hot(J, pc, L);
#else
    jit_owner_l_rel(J, L);
    lj_trace_hot(J, pc);
#endif
    lj_assertG(L->top - L->base == delta,
	       "unbalanced stack after hot call");
    goto out;
  } else if (lj_trace_state_load(J) != LJ_TRACE_IDLE &&
	     jit_owner_l_acq(J) == L &&
	     lj_jit_token_held_l(L, J) &&
	     lj_trace_state_load(J) != LJ_TRACE_IDLE &&
	     !(lj_tg_hookmask_combined_load(g, L2TG(L)) &
	       (HOOK_GC|HOOK_VMEVENT))) {
#ifdef LUA_USE_ASSERT
    ptrdiff_t delta = L->top - L->base;
#endif
    /* Record the FUNC* bytecodes, too. */
    lj_trace_ins(J, pc-1);  /* The interpreter bytecode PC is offset by 1. */
    lj_assertG(L->top - L->base == delta,
	       "unbalanced stack after hot instruction");
  }
#endif
  hookmask = lj_tg_hookmask_combined_load(g, L2TG(L));
  if ((hookmask & LUA_MASKCALL)) {
    int i, nmissing = missing;
    if (missing > 0)  /* Add missing parameters. */
      call_fill_missing(L, &missing);
    callhook(L, LUA_HOOKCALL, -1);
    /* Preserve modifications of missing parameters by lua_setlocal(). */
    for (i = 0; i < nmissing && tvisnil(L->top - 1); i++)
      L->top--;
  }
#if LJ_HASJIT
out:
#endif
  op = bc_op(pc[-1]);  /* Get FUNC* op. */
#if LJ_HASJIT
  /* Use the non-hotcounting variants if JIT is off or while recording. */
  if ((!(jit_flags_acq(J) & JIT_F_ON) ||
       lj_trace_state_load(J) != LJ_TRACE_IDLE) &&
      (op == BC_FUNCF || op == BC_FUNCV))
    op = (BCOp)((int)op+(int)BC_IFUNCF-(int)BC_FUNCF);
#endif
  ERRNO_RESTORE
  return makeasmfunc(lj_bc_ofs[op]);  /* Return static dispatch target. */
}

#if LJ_HASJIT
/* Stitch a new trace. */
#if LJ_TARGET_X64 || LJ_TARGET_ARM64
void LJ_FASTCALL lj_dispatch_stitch(jit_State *J, const BCIns *pc, lua_State *L,
				    TraceNo traceno)
#else
void LJ_FASTCALL lj_dispatch_stitch(jit_State *J, const BCIns *pc)
#endif
{
#if !(LJ_TARGET_X64 || LJ_TARGET_ARM64)
  lua_State *L = jit_owner_l_acq(J);
#endif
  if (!(lj_tg_hookmask_combined_load(J2G(J), L2TG(L)) & HOOK_VMEVENT)) {
    ERRNO_SAVE
    void *cf = cframe_raw(L->cframe);
    const BCIns *oldpc = cframe_pc(cf);
    setcframe_pc(cf, pc);
    /* Before dispatch, have to bias PC by 1. */
    L->top = L->base + cur_topslot(curr_proto(L), pc+1, cframe_multres_n(cf));
#if LJ_TARGET_X64 || LJ_TARGET_ARM64
    lj_trace_stitch(J, pc-1, L, traceno);  /* Point to the CALL instruction. */
#else
    lj_trace_stitch(J, pc-1);  /* Point to the CALL instruction. */
#endif
    setcframe_pc(cf, oldpc);
    ERRNO_RESTORE
  }
}
#endif

#if LJ_HASPROFILE
/* Profile dispatch. */
void LJ_FASTCALL lj_dispatch_profile(lua_State *L, const BCIns *pc)
{
  ERRNO_SAVE
  GCfunc *fn = curr_func(L);
  GCproto *pt = funcproto(fn);
  void *cf = cframe_raw(L->cframe);
  const BCIns *oldpc = cframe_pc(cf);
  global_State *g;
  setcframe_pc(cf, pc);
  L->top = L->base + cur_topslot(pt, pc, cframe_multres_n(cf));
  lj_profile_interpreter(L);
  setcframe_pc(cf, oldpc);
  g = G(L);
  lj_tg_setcur_L(g, L);
  setvmstate(g, INTERP);
  ERRNO_RESTORE
}
#endif
