/*
** Focused dormant-path test for IR_XSAVE snapshot identity, allocation
** materialization and x64 TG-private root staging.
*/

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_ccall.h"
#include "lj_dispatch.h"
#include "lj_gc2.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_oserr.h"
#include "lj_state.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#ifndef LJ_XSAVE_TEST_HELPERS
#error "t-jit-xsave requires -DLJ_XSAVE_TEST_HELPERS"
#endif

static int ir_is_alloc(IROp op)
{
  return op == IR_TNEW || op == IR_TDUP || op == IR_CNEW || op == IR_CNEWI;
}

static int ref_reaches_alloc(IRIns *ir, IRRef nins, IRRef ref, unsigned depth)
{
  uint8_t mode;
  if (depth == 0 || irref_isk(ref) || ref < REF_FIRST || ref >= nins)
    return 0;
  if (ir_is_alloc((IROp)ir[ref].o))
    return 1;
  mode = lj_ir_mode[ir[ref].o];
  return (irm_op1(mode) == IRMref &&
	  ref_reaches_alloc(ir, nins, ir[ref].op1, depth - 1)) ||
	 (irm_op2(mode) == IRMref &&
	  ref_reaches_alloc(ir, nins, ir[ref].op2, depth - 1));
}

static GCtrace *find_xsave_trace(jit_State *J)
{
  TraceNo traceno;
  for (traceno = 1; traceno < trace_sizetrace_acq(J); traceno++) {
    GCtrace *T = traceref(J, traceno);
    IRIns *ir;
    IRRef ref, nins;
    int has_loop = 0, has_xsave = 0, has_alloc = 0;
    if (!T || trace_traceno_acq(T) != traceno)
      continue;
    ir = trace_ir_acq(T);
    nins = trace_nins_acq(T);
    for (ref = REF_FIRST; ref < nins; ref++) {
      has_loop |= ir[ref].o == IR_LOOP;
      has_xsave |= ir[ref].o == IR_XSAVE;
      has_alloc |= ir_is_alloc((IROp)ir[ref].o);
    }
    if (has_loop && has_xsave && has_alloc)
      return T;
  }
  return NULL;
}

static GCtrace *find_xsave_retf_trace(jit_State *J)
{
  TraceNo traceno;
  for (traceno = 1; traceno < trace_sizetrace_acq(J); traceno++) {
    GCtrace *T = traceref(J, traceno);
    IRIns *ir;
    IRRef ref, nins, xsave = 0;
    if (!T || trace_traceno_acq(T) != traceno)
      continue;
    ir = trace_ir_acq(T);
    nins = trace_nins_acq(T);
    for (ref = REF_FIRST; ref < nins; ref++) {
      if (ir[ref].o == IR_XSAVE && xsave == 0)
	xsave = ref;
      if (ir[ref].o == IR_RETF && xsave != 0 && xsave < ref)
	return T;
    }
  }
  return NULL;
}

static BCReg xsave_baseslot(GCtrace *T, SnapShot *snap, int *gotframe)
{
  SnapEntry *map = &trace_snapmap_acq(T)[snap_mapofs_acq(snap)];
  MSize n;
  *gotframe = 0;
  for (n = snap_nent_acq(snap); n > 0; n--) {
    SnapEntry sn = snapentry_acq(&map[n-1]);
    if ((sn & SNAP_FRAME)) {
      *gotframe = 1;
      return snap_slot(sn) - LJ_FR2;
    }
  }
  return 0;
}

static void assert_xsave_retf_shape(GCtrace *T)
{
  IRIns *ir = trace_ir_acq(T);
  IRRef ref, nins = trace_nins_acq(T), xsave = 0;
  unsigned ordered = 0;
  for (ref = REF_FIRST; ref < nins; ref++) {
    if (ir[ref].o == IR_XSAVE && xsave == 0)
      xsave = ref;
    if (ir[ref].o == IR_RETF && xsave != 0 && xsave < ref)
      ordered++;
  }
  assert(ordered != 0);
}

static void assert_xsave_trace_shape(GCtrace *T)
{
  IRIns *ir = trace_ir_acq(T);
  SnapShot *snaps = trace_snap_acq(T);
  SnapEntry *snapmap = trace_snapmap_acq(T);
  IRRef ref, loopref = 0, nins = trace_nins_acq(T);
  SnapNo nsnap = trace_nsnap_acq(T), sn;
  unsigned nalloc = 0, nallocroot = 0, nstore = 0;
  unsigned nxsave = 0, npre = 0, ncopy = 0, ninlined = 0;

  for (ref = REF_FIRST; ref < nins; ref++) {
    IROp op = (IROp)ir[ref].o;
    if (op == IR_LOOP)
      loopref = ref;
    if (ir_is_alloc(op)) {
      nalloc++;
      assert(ir[ref].r != RID_SINK && ir[ref].r != RID_SUNK);
    }
    if (op == IR_ASTORE || op == IR_HSTORE || op == IR_FSTORE ||
	op == IR_XSTORE || op == IR_NEWREF) {
      nstore++;
      assert(ir[ref].r != RID_SINK && ir[ref].r != RID_SUNK);
    }
  }
  assert(loopref != 0);
  assert(nalloc != 0);
  assert(nstore != 0);

  for (ref = REF_FIRST; ref < nins; ref++) {
    unsigned matches = 0;
    if (ir[ref].o != IR_XSAVE)
      continue;
    nxsave++;
    if (ref < loopref) npre++;
    if (ref > loopref) ncopy++;
    for (sn = 0; sn < nsnap; sn++)
      if (snap_ref_acq(&snaps[sn]) == ref)
	matches++;
    /* The recorder marker and its LOOP-substituted copy must each retain an
    ** assembler-position snapshot instead of sharing a literal snap number. */
    assert(matches != 0);
  }
  assert(nxsave >= 2 && npre != 0 && ncopy != 0);

  for (sn = 0; sn < nsnap; sn++) {
    SnapShot *snap = &snaps[sn];
    IRRef snapref = snap_ref_acq(snap);
    MSize i, nent;
    SnapEntry *map;
    if (snapref < REF_FIRST || snapref >= nins ||
	ir[snapref].o != IR_XSAVE)
      continue;
    {
      int gotframe;
      BCReg baseslot = xsave_baseslot(T, snap, &gotframe);
      if (gotframe && baseslot != 0)
	ninlined++;
    }
    map = &snapmap[snap_mapofs_acq(snap)];
    nent = snap_nent_acq(snap);
    for (i = 0; i < nent; i++)
      if (ref_reaches_alloc(ir, nins, snap_ref(snapentry_acq(&map[i])), 64))
	nallocroot++;
  }
  /* At least one marker snapshot must own the otherwise sinkable table. */
  assert(nallocroot != 0);
  /* Exercise the real inlined-call layout. The separate hot-call trace below
  ** proves the distinct XSAVE-before-RETF backwards-allocation case. */
  assert(ninlined != 0);
}

static int trace_has_fastfunc_snapshot(global_State *g, GCtrace *T)
{
  SnapShot *snap = trace_snap_acq(T);
  SnapEntry *snapmap = trace_snapmap_acq(T);
  uintptr_t lo = (uintptr_t)(void *)&G2GG(g)->bcff[0];
  uintptr_t hi = (uintptr_t)(void *)&G2GG(g)->bcff[GG_NUM_ASMFF];
  SnapNo i;
  for (i = 0; i < trace_nsnap_acq(T); i++) {
    const BCIns *pc = snap_pc_acq(
	&snapmap[snap_mapofs_acq(&snap[i]) + snap_nent_acq(&snap[i])]);
    uintptr_t p = (uintptr_t)(const void *)pc;
    if (p >= lo && p < hi && ((p - lo) % sizeof(BCIns)) == 0)
      return 1;
  }
  return 0;
}

static GCproto *find_nonstart_snapshot_owner(lua_State *L, GCtrace *T)
{
  SnapShot *snap = trace_snap_acq(T);
  SnapEntry *snapmap = trace_snapmap_acq(T);
  GCproto *startpt = trace_startpt_acq(T);
  GCproto *result = NULL;
  int owner;
  lua_getglobal(L, "__xsave_frame_owners");
  for (owner = 1; owner <= 5 && !result; owner++) {
    GCproto *pt;
    SnapNo i;
    lua_rawgeti(L, -1, owner);
    if (!tvisfunc(L->top - 1) || !isluafunc(funcV(L->top - 1))) {
      lua_pop(L, 1);
      continue;
    }
    pt = funcproto(funcV(L->top - 1));
    if (pt != startpt) {
      uintptr_t bc = (uintptr_t)(const void *)proto_bc(pt);
      uintptr_t end = bc + (uintptr_t)pt->sizebc * sizeof(BCIns);
      for (i = 0; i < trace_nsnap_acq(T); i++) {
	const BCIns *pc = snap_pc_acq(
	  &snapmap[snap_mapofs_acq(&snap[i]) + snap_nent_acq(&snap[i])]);
	uintptr_t p = (uintptr_t)(const void *)pc;
	if (p >= bc && p < end && ((p - bc) % sizeof(BCIns)) == 0) {
	  result = pt;
	  break;
	}
      }
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  return result;
}

static GCobj *find_unmarked_trace_kgc(global_State *g, GCtrace *T)
{
  IRIns *irbase = trace_ir_acq(T);
  IRRef ref;
  for (ref = trace_nk_acq(T); ref < REF_TRUE; ref++) {
    IRIns ir = ir_load_acq(&irbase[ref]);
    if (ir.o == IR_KGC) {
      GCobj *o = ir_kgc_load_acq(&irbase[ref]);
      if (o && lj_gc2_ismarked(g, o) == 0)
	return o;
    }
    if (irt_is64(ir.t) && ir.o != IR_KNULL)
      ref++;
  }
  return NULL;
}

static void native_frame_init(LJFFINativeFrame *frame, lua_State *L,
			      GCtrace *T, TraceNo traceno)
{
  memset(frame, 0, sizeof(*frame));
  lj_ffi_native_frame_trace_rel(frame, T);
  lj_ffi_native_frame_L_rel(frame, L);
  lj_ffi_native_frame_func_rel(frame, (void *)(uintptr_t)1u);
  lj_ffi_native_frame_root_offset_rel(frame,
	(uint64_t)savestack(L, L->base));
  lj_ffi_native_frame_base_offset_rel(frame,
	(uint64_t)savestack(L, L->base));
  lj_ffi_native_frame_top_offset_rel(frame,
	(uint64_t)savestack(L, L->top));
  lj_ffi_native_frame_jit_base_offset_rel(frame,
	(uint64_t)savestack(L, L->base));
  lj_ffi_native_frame_trace_no_rel(frame, (uint32_t)traceno);
  lj_ffi_native_frame_flags_rel(frame,
	LJ_FFI_NATIVE_FRAME_F_SYNCHRONIZED |
	LJ_FFI_NATIVE_FRAME_F_ACTIVE);
}

static void xsave_stage(TGState *tg, TValue *root, uint32_t baseslot,
			uint32_t nslots)
{
  la_storeptr_rel((void **)&tg->ffi_xsave_root, root);
  la_store32_rel(&tg->ffi_xsave_baseslot, baseslot);
  la_store32_rel(&tg->ffi_xsave_nslots, nslots);
}

enum {
  FINISH_HOOK_NONE,
  FINISH_HOOK_CALLBACK,
  FINISH_HOOK_EPOCH
};

static uint32_t finish_hook_mode;
static uint32_t finish_hook_calls;
static uint64_t finish_hook_old_epoch;

static void native_finish_hook(TGState *tg)
{
  assert((lj_ffi_native_frame_sequence_acq(tg) & 1u) != 0);
  finish_hook_calls++;
  if (finish_hook_mode == FINISH_HOOK_CALLBACK) {
    ccallback_slot_rel(&tg->cb, 7);
  } else if (finish_hook_mode == FINISH_HOOK_EPOCH) {
    finish_hook_old_epoch = lj_tg_hs_epoch_ack_acq(tg);
    lj_tg_hs_epoch_ack_rel(tg, finish_hook_old_epoch + 1u);
  } else {
    assert(0);
  }
}

static void test_xsave_native_owner_lifecycle(lua_State *L, global_State *g,
				       TGState *tg, GCtrace *T)
{
  LJFFINativeFrameSnapshot snapshot;
  TValue *stack = tvref(L->stack);
  TValue *root = (TValue *)la_loadptr_acq(
    (void *const *)&tg->ffi_xsave_root);
  uint32_t baseslot = la_load32_acq(&tg->ffi_xsave_baseslot);
  uint32_t nslots = la_load32_acq(&tg->ffi_xsave_nslots);
  TValue *base = root + baseslot;
  TValue *top = root + nslots - 1u - LJ_FR2;
  TValue *old_jitbase = lj_tg_load_jit_base(tg);
  void *old_func = lj_tg_ffi_call_func_acq(tg);
  MSize old_slot = ccallback_slot_acq(&tg->cb);
  uint8_t old_stopreq = ccallback_native_had_stopreq_acq(&tg->cb);
  void *func = (void *)(uintptr_t)UINT64_C(0x12345000);
  uint64_t seq = lj_ffi_native_frame_sequence_acq(tg);
  uint32_t pins = trace_native_pins_acq(T);
  uint32_t actions;

  assert(old_jitbase == NULL);
  assert(lj_tg_in_native_acq(tg) == 0);
  lj_tg_store_jit_base(tg, base);

  /* A malformed pending extent requests a pre-call side exit and preserves
  ** every staged/mirror/lifetime word, including the foreign error pair. */
  la_store32_rel(&tg->ffi_xsave_nslots, 0);
  errno = EDOM;
#if LJ_TARGET_WINDOWS
  SetLastError((DWORD)0x13572468u);
#endif
  assert(lj_ffi_native_trace_enter(L, T, func) == 0);
  assert(errno == EDOM);
#if LJ_TARGET_WINDOWS
  assert(GetLastError() == (DWORD)0x13572468u);
#endif
  assert(la_loadptr_acq((void *const *)&tg->ffi_xsave_root) == root);
  assert(la_load32_acq(&tg->ffi_xsave_baseslot) == baseslot);
  assert(la_load32_acq(&tg->ffi_xsave_nslots) == 0);
  assert(lj_ffi_native_frame_sequence_acq(tg) == seq);
  assert(lj_ffi_native_frame_depth_acq(tg) == 0);
  assert(trace_native_pins_acq(T) == pins);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_ffi_call_func_acq(tg) == old_func);
  assert(ccallback_slot_acq(&tg->cb) == old_slot);
  assert(ccallback_native_had_stopreq_acq(&tg->cb) == old_stopreq);
  xsave_stage(tg, root, baseslot, nslots);

  errno = EDOM;
#if LJ_TARGET_WINDOWS
  SetLastError((DWORD)0x24681357u);
#endif
  assert(lj_ffi_native_trace_enter(L, T, func) == 1);
  assert(errno == EDOM);
#if LJ_TARGET_WINDOWS
  assert(GetLastError() == (DWORD)0x24681357u);
#endif
  assert(lj_ffi_native_frame_sequence_acq(tg) == seq + 2u);
  assert(lj_ffi_native_frame_depth_acq(tg) == 1);
  assert(lj_tg_in_native_acq(tg) == 1);
  assert(trace_native_pins_acq(T) == pins + 1u);
  assert(la_loadptr_acq((void *const *)&tg->ffi_xsave_root) == NULL);
  assert(la_load32_acq(&tg->ffi_xsave_baseslot) == 0);
  assert(la_load32_acq(&tg->ffi_xsave_nslots) == 0);
  assert(lj_tg_ffi_call_func_acq(tg) == func);
  assert(ccallback_slot_acq(&tg->cb) == (MSize)~0u);
  assert(lj_ffi_native_frame_snapshot(tg, &snapshot) ==
    LJ_FFI_NATIVE_FRAME_SNAPSHOT_STABLE);
  assert(snapshot.depth == 1 && snapshot.sequence == seq + 2u);
  assert(lj_ffi_native_frame_trace_acq(&snapshot.frame[0]) == T);
  assert(lj_ffi_native_frame_L_acq(&snapshot.frame[0]) == L);
  assert(lj_ffi_native_frame_func_acq(&snapshot.frame[0]) == func);
  assert(lj_ffi_native_frame_root_offset_acq(&snapshot.frame[0]) ==
    (uint64_t)((char *)root - (char *)stack));
  assert(lj_ffi_native_frame_base_offset_acq(&snapshot.frame[0]) ==
    (uint64_t)((char *)base - (char *)stack));
  assert(lj_ffi_native_frame_top_offset_acq(&snapshot.frame[0]) ==
    (uint64_t)((char *)top - (char *)stack));
  assert(lj_ffi_native_frame_jit_base_offset_acq(&snapshot.frame[0]) ==
    (uint64_t)((char *)base - (char *)stack));
  assert(lj_ffi_native_frame_trace_no_acq(&snapshot.frame[0]) ==
    (uint32_t)trace_traceno_acq(T));

  errno = ERANGE;  /* Simulated immediate foreign return pair. */
#if LJ_TARGET_WINDOWS
  SetLastError((DWORD)0x55aa33ccu);
#endif
  actions = lj_ffi_native_trace_leave(L);
  assert(actions == 0);
  assert(errno == ERANGE);
#if LJ_TARGET_WINDOWS
  assert(GetLastError() == (DWORD)0x55aa33ccu);
#endif
  assert(lj_ffi_native_frame_depth_acq(tg) == 0);
  assert(lj_ffi_native_frame_snapshot(tg, &snapshot) ==
    LJ_FFI_NATIVE_FRAME_SNAPSHOT_EMPTY);
  assert(lj_ffi_native_frame_sequence_acq(tg) == seq + 4u);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(trace_native_pins_acq(T) == pins);
  assert(lj_tg_ffi_call_func_acq(tg) == old_func);
  assert(ccallback_slot_acq(&tg->cb) == old_slot);
  assert(ccallback_native_had_stopreq_acq(&tg->cb) == old_stopreq);

  /* Callback observation is declaration-independent. It transfers the exact
  ** body pin to a stable POSTCALL frame until the caller-state trace exit has
  ** completed protected snapshot restoration. */
  xsave_stage(tg, root, baseslot, nslots);
  assert(lj_ffi_native_trace_enter(L, T, func) == 1);
  finish_hook_mode = FINISH_HOOK_CALLBACK;
  finish_hook_calls = 0;
  lj_ffi_native_trace_test_set_finish_hook(native_finish_hook);
  errno = EOVERFLOW;
#if LJ_TARGET_WINDOWS
  SetLastError((DWORD)0x778899aau);
#endif
  actions = lj_ffi_native_trace_leave(L);
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(finish_hook_calls == 1);
  assert((actions & LJ_FFI_NATIVE_LEAVE_FORCE_EXIT) != 0);
  assert(errno == EOVERFLOW);
#if LJ_TARGET_WINDOWS
  assert(GetLastError() == (DWORD)0x778899aau);
#endif
  assert(lj_ffi_native_frame_depth_acq(tg) == 1);
  assert(lj_ffi_native_frame_snapshot(tg, &snapshot) ==
    LJ_FFI_NATIVE_FRAME_SNAPSHOT_STABLE);
  assert(snapshot.depth == 1);
  assert(lj_ffi_native_frame_trace_acq(&snapshot.frame[0]) == T);
  assert(lj_ffi_native_frame_L_acq(&snapshot.frame[0]) == L);
  assert((lj_ffi_native_frame_flags_acq(&snapshot.frame[0]) &
    (LJ_FFI_NATIVE_FRAME_F_SYNCHRONIZED |
     LJ_FFI_NATIVE_FRAME_F_ACTIVE |
     LJ_FFI_NATIVE_FRAME_F_CALLBACK_SEEN |
     LJ_FFI_NATIVE_FRAME_F_POSTCALL)) ==
    (LJ_FFI_NATIVE_FRAME_F_SYNCHRONIZED |
     LJ_FFI_NATIVE_FRAME_F_CALLBACK_SEEN |
     LJ_FFI_NATIVE_FRAME_F_POSTCALL));
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(trace_native_pins_acq(T) == pins + 1u);
  assert(lj_tg_ffi_call_func_acq(tg) == old_func);
  assert(ccallback_slot_acq(&tg->cb) == old_slot);

  /* POSTCALL is a lifetime handoff, never a parked-stack scan certificate. */
  assert(lj_gc2_test_scan_ffi_native_frames(g, tg) == 0);
  errno = EOVERFLOW;
#if LJ_TARGET_WINDOWS
  SetLastError((DWORD)0xaabbccddu);
#endif
  assert(lj_ffi_native_trace_exit_cleanup(L, T,
	(uint32_t)lj_ffi_native_frame_trace_no_acq(&snapshot.frame[0])) == 1);
  assert(errno == EOVERFLOW);
#if LJ_TARGET_WINDOWS
  assert(GetLastError() == (DWORD)0xaabbccddu);
#endif
  assert(lj_ffi_native_frame_depth_acq(tg) == 0);
  assert(lj_ffi_native_frame_snapshot(tg, &snapshot) ==
    LJ_FFI_NATIVE_FRAME_SNAPSHOT_EMPTY);
  assert(trace_native_pins_acq(T) == pins);
  assert(lj_ffi_native_trace_exit_cleanup(L, T,
	(uint32_t)trace_traceno_acq(T)) == 0);

  /* A changed remotely acknowledged epoch takes the same non-replaying
  ** handoff, and the final epoch sample is made only after odd publication. */
  xsave_stage(tg, root, baseslot, nslots);
  assert(lj_ffi_native_trace_enter(L, T, func) == 1);
  finish_hook_mode = FINISH_HOOK_EPOCH;
  finish_hook_calls = 0;
  lj_ffi_native_trace_test_set_finish_hook(native_finish_hook);
  actions = lj_ffi_native_trace_leave(L);
  lj_ffi_native_trace_test_set_finish_hook(NULL);
  assert(finish_hook_calls == 1);
  assert((actions & LJ_FFI_NATIVE_LEAVE_FORCE_EXIT) != 0);
  lj_tg_hs_epoch_ack_rel(tg, finish_hook_old_epoch);
  assert(lj_ffi_native_frame_depth_acq(tg) == 1);
  assert(trace_native_pins_acq(T) == pins + 1u);
  assert(lj_ffi_native_trace_exit_cleanup(L, T,
	(uint32_t)trace_traceno_acq(T)) == 1);
  assert(lj_ffi_native_frame_depth_acq(tg) == 0);
  assert(trace_native_pins_acq(T) == pins);

  lj_tg_store_jit_base(tg, old_jitbase);
  /* A native leave may service a previously pending GC handshake. Keep the
  ** following mark-plane oracle independent of that lawful scheduling edge. */
  lj_gc2_cycle_to_idle(g);
}

static void test_certified_native_frame_scanner(lua_State *L, global_State *g,
					 TGState *tg, jit_State *J,
					 GCtrace *T)
{
  LJFFINativeFrame frame;
  GCtab *root;
  GCproto *startpt = trace_startpt_acq(T);
  GCproto *snapshotpt;
  GCobj *kgc;
  TraceNo traceno = trace_traceno_acq(T);
  TValue *old_jitbase = lj_tg_load_jit_base(tg);
  uint64_t attempts, invalid, retries, stable, seq;

  assert(traceno != 0 && traceref_safe(J, traceno) == T);
  assert(startpt != NULL);
  assert(trace_has_fastfunc_snapshot(g, T));
  snapshotpt = find_nonstart_snapshot_owner(L, T);
  assert(snapshotpt != NULL);
  /* Force the exact path through the HugeTab-backed stack validator/lease;
  ** generic CALLXS must work after ordinary deep-stack growth too. */
  assert(lua_checkstack(L, 3000));
  assert((size_t)L->stacksize * sizeof(TValue) > LJ_HUGE_THRESHOLD);
  lua_newtable(L);
  root = tabV(L->top - 1);
  native_frame_init(&frame, L, T, traceno);
  assert(old_jitbase == NULL);
  lj_tg_store_jit_base(tg, L->base);

  /* A stable-looking frame without its exact body lease fails before any
  ** trace-body dereference or TValue root-slot scan. Trusted L/stack geometry
  ** is validated first under the test's synchronous stability certificate. */
  attempts = gc2_ffi_native_scan_attempts_acq(g);
  invalid = gc2_ffi_native_scan_invalid_acq(g);
  assert(lj_ffi_native_frame_push(tg, &frame) == 1);
  assert(lj_gc2_test_scan_ffi_native_frames(g, tg) == 0);
  assert(gc2_ffi_native_scan_attempts_acq(g) == attempts + 1u);
  assert(gc2_ffi_native_scan_invalid_acq(g) == invalid + 1u);
  lj_ffi_native_frame_pop(tg, NULL);

  /* Raw mismatched publications are rejected by trusted pointer equality;
  ** neither deliberately invalid address may be dereferenced. */
  lj_ffi_native_frame_trace_rel(&frame, (GCtrace *)(uintptr_t)3u);
  invalid = gc2_ffi_native_scan_invalid_acq(g);
  assert(lj_ffi_native_frame_push(tg, &frame) == 1);
  assert(lj_gc2_test_scan_ffi_native_frames(g, tg) == 0);
  assert(gc2_ffi_native_scan_invalid_acq(g) == invalid + 1u);
  lj_ffi_native_frame_pop(tg, NULL);
  native_frame_init(&frame, L, T, traceno);
  lj_ffi_native_frame_L_rel(&frame, (lua_State *)(uintptr_t)5u);
  invalid = gc2_ffi_native_scan_invalid_acq(g);
  assert(lj_ffi_native_frame_push(tg, &frame) == 1);
  assert(lj_gc2_test_scan_ffi_native_frames(g, tg) == 0);
  assert(gc2_ffi_native_scan_invalid_acq(g) == invalid + 1u);
  lj_ffi_native_frame_pop(tg, NULL);

  lj_tg_store_jit_base(tg, old_jitbase);

  /* Acquire the exact lease while the live TraceVec/SMR reader is the
  ** independent lifetime proof required by lj_trace_native_pin(). */
  native_frame_init(&frame, L, T, traceno);
  lj_gc2_smr_read_enter(g);
  assert(traceref_safe(J, traceno) == T);
  assert(lj_trace_native_pin(T) == 1);
  lj_gc2_smr_read_leave(g);
  assert(trace_native_pins_acq(T) == 1u);

  /* Retirement clears T->traceno, so an ordinary queued GC2 traversal would
  ** deliberately skip this body. The pin keeps its original public slot
  ** reserved and makes the certified scanner's synchronous graph-preserve
  ** path both necessary and testable. */
  assert(lj_trace_flushall_gc(L) == 0);
  assert(trace_native_pin_closed_acq(T));
  assert(trace_traceno_acq(T) == 0);
  assert(trace_nextroot_acq(T) == traceno);
  assert(traceref_safe(J, traceno) == T);

  lj_gc2_mark_begin(g);
  /* Stopped/stitched C-call traces use these permanent global-owned PCs when
  ** no Lua prototype is recording. They are valid graph leaves too. */
  assert(lj_gc2_mark_proto_for_pc(g, &g->bc_cfunc_int) == 1);
  assert(lj_gc2_mark_proto_for_pc(g, &g->bc_cfunc_ext) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(root)) == 0);
  kgc = find_unmarked_trace_kgc(g, T);
  assert(kgc != NULL);
  lj_tg_store_jit_base(tg, L->base);
  attempts = gc2_ffi_native_scan_attempts_acq(g);
  stable = gc2_ffi_native_scan_stable_frames_acq(g);
  seq = lj_ffi_native_frame_sequence_acq(tg);
  assert(lj_ffi_native_frame_push(tg, &frame) == 1);
  assert(lj_gc2_test_scan_ffi_native_frames(g, tg) == 1);
  assert(gc2_ffi_native_scan_attempts_acq(g) == attempts + 1u);
  assert(gc2_ffi_native_scan_stable_frames_acq(g) == stable + 1u);
  assert(lj_gc2_ismarked(g, obj2gco(root)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(startpt)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(snapshotpt)) == 1);
  assert(lj_gc2_ismarked(g, kgc) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(T)) == 1);
  assert(trace_native_pins_acq(T) == 1u);
  assert(lj_ffi_native_frame_sequence_acq(tg) == seq + 2u);
  lj_ffi_native_frame_pop(tg, NULL);

  /* Checked byte geometry rejects a stable misaligned offset. */
  lj_ffi_native_frame_base_offset_rel(&frame,
	lj_ffi_native_frame_base_offset_acq(&frame) + 1u);
  invalid = gc2_ffi_native_scan_invalid_acq(g);
  assert(lj_ffi_native_frame_push(tg, &frame) == 1);
  assert(lj_gc2_test_scan_ffi_native_frames(g, tg) == 0);
  assert(gc2_ffi_native_scan_invalid_acq(g) == invalid + 1u);
  lj_ffi_native_frame_pop(tg, NULL);
  assert(trace_native_pins_acq(T) == 1u);

  attempts = gc2_ffi_native_scan_attempts_acq(g);
  lj_gc2_scan_cycle_owner_tg_roots(g, tg);
  assert(gc2_ffi_native_scan_attempts_acq(g) == attempts);
  /* A final closed unpin may make T reclaimable immediately; do not read the
  ** body or its count word after this release. */
  lj_trace_native_unpin(g, T);
  lj_tg_store_jit_base(tg, old_jitbase);

  /* Odd whole-stack publication is a transient retry and leaves output/root
  ** authority to the unchanged broad owner scan. */
  seq = lj_ffi_native_frame_sequence_acq(tg);
  retries = gc2_ffi_native_scan_retries_acq(g);
  la_store64_rel(&tg->ffi_native_seq, seq + 1u);
  assert(lj_gc2_test_scan_ffi_native_frames(g, tg) == 0);
  assert(gc2_ffi_native_scan_retries_acq(g) == retries + 1u);
  la_store64_rel(&tg->ffi_native_seq, seq + 2u);

  lj_gc2_cycle_to_idle(g);
  lua_pop(L, 1);
}

int main(void)
{
#if !LJ_TARGET_X64
  printf("t-jit-xsave SKIP: x64-only lowering\n");
  return 0;
#else
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  jit_State *J = G2J(g);
  TGState *tg = L2TG(L);
  TValue *stack;
  TValue *maxstack;
  GCtrace *T, *scan_trace;

  assert(tg->ffi_xsave_root == NULL);
  assert(tg->ffi_xsave_baseslot == 0);
  assert(tg->ffi_xsave_nslots == 0);
  tg->ffi_xsave_root = (TValue *)(uintptr_t)1;
  tg->ffi_xsave_baseslot = UINT32_MAX;
  tg->ffi_xsave_nslots = UINT32_MAX;
  ljt_lua_dostring(L,
    "_G.__xsave_chunk_owner = debug.getinfo(1, 'f').func\n"
    "local util = require('jit.util')\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1', '+sink')\n"
    "local function inner(i)\n"
    "  local t = { i }\n"
    "  t.keep = i + 1\n"
    "  local x = math.abs(-i)\n"
    "  return x + t[1] + t.keep - (3*i + 1)\n"
    "end\n"
    "local function outer(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do\n"
    "    s = s + inner(i)\n"
    "  end\n"
    "  return s\n"
    "end\n"
    "local function drive(n)\n"
    "  for i = 1, n do assert(outer(80) == 0) end\n"
    "end\n"
    "drive(40)\n"
    "assert(util.traceinfo(1), 'expected XSAVE loop trace')\n"
    "drive(40)\n"
    "local function retrec(n)\n"
    "  if n == 0 then return 0 end\n"
    "  local x = retrec(n - 1)\n"
    "  local y = math.abs(-n)\n"
    "  return x + y\n"
    "end\n"
    "for i = 1, 120 do assert(retrec(12) == 78) end\n"
    "drive(40)\n"
    /* The table-allocation trace may side-exit before its post-allocation
    ** marker under a new arena representation. Stage through a separate
    ** allocation-free marker after both shape traces have been built. */
    "local function stage(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + math.abs(-i) end\n"
    "  return s\n"
    "end\n"
    "for i = 1, 40 do assert(stage(80) == 3240) end\n"
    /* The synthetic frame is published after this chunk has returned, unlike
    ** a real native frame whose materialized Lua frames retain every inlined
    ** prototype. Keep those owners live so the checked snapshot-PC graph is a
    ** valid success fixture rather than the intended fail-closed case. */
    "_G.__xsave_frame_owners = {inner, outer, drive, retrec, stage}\n");

  scan_trace = find_xsave_trace(J);
  assert(scan_trace != NULL);
  assert_xsave_trace_shape(scan_trace);
  T = find_xsave_retf_trace(J);
  assert(T != NULL);
  assert_xsave_retf_shape(T);

  stack = mref(L->stack, TValue);
  maxstack = mref(L->maxstack, TValue);
  assert(tg->ffi_xsave_root != (TValue *)(uintptr_t)1);
  assert(tg->ffi_xsave_root >= stack && tg->ffi_xsave_root < maxstack);
  assert(tg->ffi_xsave_baseslot != UINT32_MAX);
  assert(tg->ffi_xsave_nslots != UINT32_MAX);
  /* The last marker executes inside inner(), so its complete logical root is
  ** the trace root while the current frame begins at a non-zero slot. */
  assert(tg->ffi_xsave_baseslot != 0);
  assert(tg->ffi_xsave_nslots > tg->ffi_xsave_baseslot + LJ_FR2);
  assert(tg->ffi_xsave_root + tg->ffi_xsave_nslots <= maxstack);

  test_xsave_native_owner_lifecycle(L, g, tg, scan_trace);
  test_certified_native_frame_scanner(L, g, tg, J, scan_trace);

  lua_close(L);
  printf("t-jit-xsave OK: copied snapshots materialize and stage exact roots\n");
  return 0;
#endif
}
