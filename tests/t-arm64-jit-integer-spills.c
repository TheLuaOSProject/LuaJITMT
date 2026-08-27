/*
** Native macOS ARM64 contract for integer spills in admitted root loops.
**
** Three deliberately separate roots prove the fixed reserve, the smallest
** dynamic frame, and the first useful copy-spill/overflow pressure shape.
** Calls, allocations, side traces, stitches and JFUNCF entry remain excluded.
*/

#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && \
    defined(LJ_TRACE_TEST_HELPERS)

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_dispatch.h"
#include "lj_func.h"
#include "lj_gc2.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_profile.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_trace.h"

#if !LJ_HASJIT || LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED || \
    !LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED
#error "t-arm64-jit-integer-spills requires the root-only ARM64 gate split"
#endif

#if !LJ_HASPROFILE || !LJ_PROFILE_TGLOCAL
#error "t-arm64-jit-integer-spills requires TG-local profile polling"
#endif

enum {
  FIXED_N = 23,
  MIN_DYNAMIC_N = 23,
  HEAVY_N = 26,
  SOURCE_CAPACITY = 32768
};

typedef enum SpillKind {
  SPILL_FIXED,
  SPILL_MIN_DYNAMIC,
  SPILL_HEAVY
} SpillKind;

typedef struct SpillSpec {
  const char *name;
  SpillKind kind;
  MSize nvalue;
  MSize ninvariant;
  MSize spadjust;
  MSize semantic_nins;
  MSize suffix_nins;
  MSize nsnap;
  MSize nsnapmap;
  MSize snapshot_tuples;
  MSize highest_slot;
  uint64_t ir_fingerprint;
  uint64_t snapshot_fingerprint;
} SpillSpec;

typedef struct SpillRoot {
  lua_State *L;
  global_State *g;
  jit_State *J;
  TGState *tg;
  GCproto *pt;
  GCtrace *T;
  const SpillSpec *spec;
  int32_t idle_vmstate;
  IRRef loopref;
  SnapNo loopsnap;
} SpillRoot;

/* Fingerprints are filled from a direct allocator measurement, then treated
** as part of the exact deterministic contract. */
static const SpillSpec fixed_spec = {
  "fixed-23", SPILL_FIXED, FIXED_N, 0, 0,
  4*FIXED_N+9, FIXED_N+2, 2*FIXED_N+7, 1103, 997, 3,
  UINT64_C(0x8e987d05ec986c8b), UINT64_C(0xa5e15ce0c3358d84)
};

static const SpillSpec min_dynamic_spec = {
  "dynamic-23-plus-k", SPILL_MIN_DYNAMIC, MIN_DYNAMIC_N, 1, 16,
  4*MIN_DYNAMIC_N+10, MIN_DYNAMIC_N, 2*MIN_DYNAMIC_N+7, 1105, 999, 5,
  UINT64_C(0x9daea8d07564bb57), UINT64_C(0x3074ad891371262f)
};

static const SpillSpec heavy_spec = {
  "heavy-26", SPILL_HEAVY, HEAVY_N, 0, 128,
  4*HEAVY_N+9, 1, 2*HEAVY_N+7, 1361, 1243, 35,
  UINT64_C(0x0e434c1ea83db142), UINT64_C(0x697804b1a9938ca1)
};

static void appendf(char **cursor, size_t *left, const char *fmt, ...)
{
  va_list ap;
  int n;
  va_start(ap, fmt);
  n = vsnprintf(*cursor, *left, fmt, ap);
  va_end(ap);
  assert(n >= 0 && (size_t)n < *left);
  *cursor += n;
  *left -= (size_t)n;
}

static void build_chunk(const SpillSpec *spec, char *source, size_t capacity)
{
  char *cursor = source;
  size_t left = capacity;
  MSize j;

  appendf(&cursor, &left,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1000','maxtrace=2'); "
    "function __arm64_spill_loop(n");
  for (j = 1; j <= spec->nvalue; j++)
    appendf(&cursor, &left, ",a%u", (unsigned)j);
  if (spec->ninvariant)
    appendf(&cursor, &left, ",k");
  appendf(&cursor, &left, ") local i=0 while i<n do i=i+1; ");
  for (j = 1; j <= spec->nvalue; j++) {
    if (spec->ninvariant && j == 1)
      appendf(&cursor, &left, "a1=a1+k; ");
    else
      appendf(&cursor, &left, "a%u=a%u+%u; ",
	      (unsigned)j, (unsigned)j, (unsigned)j);
  }
  appendf(&cursor, &left, "end return i");
  for (j = 1; j <= spec->nvalue; j++)
    appendf(&cursor, &left, ",a%u", (unsigned)j);
  if (spec->ninvariant)
    appendf(&cursor, &left, ",k");
  appendf(&cursor, &left, " end");
}

static void run_lua(lua_State *L, const char *chunk)
{
  int status = luaL_dostring(L, chunk);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 integer-spill chunk failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static GCproto *spill_proto(lua_State *L)
{
  GCfunc *fn;
  GCproto *pt;
  lua_getglobal(L, "__arm64_spill_loop");
  assert(lua_isfunction(L, -1));
  fn = funcV(L->top-1);
  assert(isluafunc(fn));
  pt = funcproto(fn);
  lua_pop(L, 1);
  return pt;
}

static void push_vector(lua_State *L, const SpillSpec *spec, lua_Integer n,
	int negative, int overflow)
{
  MSize j;
  lua_pushinteger(L, n);
  for (j = 1; j <= spec->nvalue; j++) {
    lua_Integer seed = negative ? -(1000+(lua_Integer)j) :
				 1000+(lua_Integer)j;
    if (overflow && j == 2)
      seed = 2147483644LL;
    lua_pushinteger(L, seed);
  }
  if (spec->ninvariant)
    lua_pushinteger(L, 1);
}

static int call_vector(lua_State *L, const SpillSpec *spec, lua_Integer n,
	int negative, int overflow)
{
  int base = lua_gettop(L);
  void *saved_cframe = L->cframe;
  int nargs = 1+(int)spec->nvalue+(int)spec->ninvariant;
  int nresults = 1+(int)spec->nvalue+(int)spec->ninvariant;
  int status;
  assert(lua_checkstack(L, nargs+nresults+8));
  lua_getglobal(L, "__arm64_spill_loop");
  assert(lua_isfunction(L, -1));
  push_vector(L, spec, n, negative, overflow);
  status = lua_pcall(L, nargs, nresults, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 integer-spill call failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_gettop(L)-base == nresults);
  assert(L->cframe == saved_cframe);
  return base+1;
}

static void expect_vector(lua_State *L, const SpillSpec *spec, int first,
	lua_Integer n, int negative)
{
  MSize j;
  assert(lua_tointeger(L, first) == n);
  for (j = 1; j <= spec->nvalue; j++) {
    lua_Integer seed = negative ? -(1000+(lua_Integer)j) :
				 1000+(lua_Integer)j;
    lua_Integer step = (spec->ninvariant && j == 1) ? 1 : (lua_Integer)j;
    assert(lua_tointeger(L, first+(int)j) == seed+n*step);
  }
  if (spec->ninvariant)
    assert(lua_tointeger(L, first+1+(int)spec->nvalue) == 1);
  lua_settop(L, first-1);
}

static IRRef expected_semantic_end(const SpillSpec *spec)
{
  return REF_FIRST+(IRRef)spec->semantic_nins;
}

static RegSP effective_snapshot_regsp(const GCtrace *T, IRRef semantic_end,
	SnapNo snapno, IRRef ref)
{
  const IRIns *ir = trace_ir_acq(T);
  IRRef renref;
  RegSP rs = ir[ref].prev;
  for (renref = trace_nins_acq(T); renref-- > semantic_end; ) {
    const IRIns *ren = &ir[renref];
    if (ren->o != IR_RENAME)
      break;
    if (ren->op1 == ref && ren->op2 <= snapno)
      rs = ren->prev;
  }
  return rs;
}

static uint64_t fingerprint_word(uint64_t h, uint64_t word)
{
  unsigned i;
  for (i = 0; i < 8; i++) {
    h ^= (uint8_t)(word >> (i*8));
    h *= UINT64_C(1099511628211);
  }
  return h;
}

static uint64_t ir_fingerprint(const GCtrace *T)
{
  const IRIns *ir = trace_ir_acq(T);
  IRRef ref;
  uint64_t h = UINT64_C(1469598103934665603);
  for (ref = REF_BASE; ref < trace_nins_acq(T); ref++) {
    h = fingerprint_word(h, ref-REF_BASE);
    h = fingerprint_word(h, ir[ref].o);
    h = fingerprint_word(h, ir[ref].t.irt);
    h = fingerprint_word(h, ir[ref].op1);
    h = fingerprint_word(h, ir[ref].op2);
    h = fingerprint_word(h, ir[ref].r);
    h = fingerprint_word(h, ir[ref].s);
    h = fingerprint_word(h, ir[ref].prev);
  }
  return h;
}

static uint64_t snapshot_fingerprint(const GCtrace *T, IRRef semantic_end,
	MSize *ntuplesp)
{
  const SnapShot *snap = trace_snap_acq(T);
  const SnapEntry *snapmap = trace_snapmap_acq(T);
  uint64_t h = UINT64_C(1469598103934665603);
  MSize ntuples = 0;
  SnapNo snapno;
  for (snapno = 0; snapno < trace_nsnap_acq(T); snapno++) {
    MSize n;
    h = fingerprint_word(h, snapno);
    h = fingerprint_word(h, snap_ref_acq(&snap[snapno])-REF_FIRST);
    h = fingerprint_word(h, snap_nent_acq(&snap[snapno]));
    for (n = 0; n < snap_nent_acq(&snap[snapno]); n++) {
      SnapEntry sn = snapentry_acq(
	&snapmap[snap_mapofs_acq(&snap[snapno])+n]);
      IRRef ref = snap_ref(sn);
      h = fingerprint_word(h, n);
      h = fingerprint_word(h, sn);
      if (!irref_isk(ref) && !(sn & SNAP_FRAME)) {
	RegSP rs = effective_snapshot_regsp(T, semantic_end, snapno, ref);
	h = fingerprint_word(h, ref-REF_FIRST);
	h = fingerprint_word(h, regsp_reg(rs));
	h = fingerprint_word(h, regsp_spill(rs));
	ntuples++;
      }
    }
  }
  if (ntuplesp) *ntuplesp = ntuples;
  return h;
}

static void expect_ir_ins(const IRIns *ir, IRRef ref, IROp op, uint8_t type,
	IRRef op1, IRRef op2)
{
  if (ir[ref].o != op || ir[ref].t.irt != type || ir[ref].op1 != op1 ||
      ir[ref].op2 != op2)
    fprintf(stderr, "IR %u got op=%u type=%u op1=%u op2=%u; "
	    "want op=%u type=%u op1=%u op2=%u\n",
	    (unsigned)(ref-REF_FIRST), (unsigned)ir[ref].o,
	    (unsigned)ir[ref].t.irt, (unsigned)ir[ref].op1,
	    (unsigned)ir[ref].op2, (unsigned)op, (unsigned)type,
	    (unsigned)op1, (unsigned)op2);
  assert(ir[ref].o == op);
  assert(ir[ref].t.irt == type);
  assert(ir[ref].op1 == op1);
  assert(ir[ref].op2 == op2);
}

static IRRef find_kint(const GCtrace *T, int32_t value)
{
  const IRIns *ir = trace_ir_acq(T);
  IRRef ref;
  for (ref = trace_nk_acq(T); ref < REF_TRUE; ref++)
    if (ir[ref].o == IR_KINT && ir[ref].t.irt == IRT_INT &&
	ir[ref].i == value)
      return ref;
  assert(!"missing integer constant");
  return 0;
}

static IRRef expected_snapshot_ref(const SpillSpec *spec, SnapNo snapno)
{
  MSize n = spec->nvalue;
  MSize shift = spec->ninvariant ? 1u : 0u;
  if (snapno == 0)
    return REF_FIRST;
  if (snapno <= n+1u)
    return REF_FIRST+(IRRef)(n+1u+shift+snapno-1u);
  if (snapno == n+2u)
    return REF_FIRST+(IRRef)(2u*n+2u+shift);
  if (snapno == n+3u)
    return REF_FIRST+(IRRef)(2u*n+3u+shift);
  if (snapno == n+4u)
    return REF_FIRST+(IRRef)(2u*n+4u+shift);
  if (snapno <= 2u*n+5u)
    return REF_FIRST+(IRRef)(2u*n+6u+shift+snapno-(n+5u));
  assert(snapno == 2u*n+6u);
  return REF_FIRST+(IRRef)(3u*n+7u+shift);
}

static const Reg fixed_rename_regs[] = {
  RID_X3, RID_X4, RID_X5, RID_X6, RID_X7, RID_X8, RID_X9, RID_X10,
  RID_X11, RID_X12, RID_X13, RID_X14, RID_X15, RID_X16, RID_X17,
  RID_X19, RID_X20, RID_X21, RID_X23, RID_X24, RID_X26, RID_X27,
  RID_X28, RID_X1, RID_X2
};

static const Reg dynamic_rename_regs[] = {
  RID_X4, RID_X5, RID_X6, RID_X7, RID_X8, RID_X9, RID_X10, RID_X11,
  RID_X12, RID_X13, RID_X14, RID_X15, RID_X16, RID_X17, RID_X19,
  RID_X20, RID_X21, RID_X23, RID_X24, RID_X26, RID_X27, RID_X28,
  RID_X1
};

static void expect_rename_suffix(const SpillRoot *root, IRRef semantic_end)
{
  const SpillSpec *spec = root->spec;
  const IRIns *ir = trace_ir_acq(root->T);
  MSize n;
  if (spec->kind == SPILL_HEAVY) {
    assert(spec->suffix_nins == 1);
    assert(ir[semantic_end].o == IR_NOP);
    return;
  }
  if (spec->kind == SPILL_FIXED) {
    assert(spec->suffix_nins == sizeof(fixed_rename_regs) /
	  sizeof(fixed_rename_regs[0]));
    for (n = 0; n < spec->suffix_nins; n++) {
      IRRef expected_op1 = n < FIXED_N+1u ?
	REF_FIRST+FIXED_N+1u+(IRRef)n : REF_FIRST+FIXED_N+1u;
      assert(ir[semantic_end+(IRRef)n].op1 == expected_op1);
      assert(ir[semantic_end+(IRRef)n].r == fixed_rename_regs[n]);
    }
  } else {
    assert(spec->kind == SPILL_MIN_DYNAMIC);
    assert(spec->suffix_nins == sizeof(dynamic_rename_regs) /
	  sizeof(dynamic_rename_regs[0]));
    for (n = 0; n < spec->suffix_nins; n++) {
      assert(ir[semantic_end+(IRRef)n].op1 ==
	     REF_FIRST+MIN_DYNAMIC_N+3u+(IRRef)n);
      assert(ir[semantic_end+(IRRef)n].r == dynamic_rename_regs[n]);
    }
  }
}

static void expect_semantics(const SpillRoot *root)
{
  const SpillSpec *spec = root->spec;
  const IRIns *ir = trace_ir_acq(root->T);
  MSize n = spec->nvalue;
  MSize shift = spec->ninvariant ? 1u : 0u;
  IRRef semantic_end = expected_semantic_end(spec);
  IRRef s_i = REF_FIRST;
  IRRef s_k = 0;
  IRRef left_i = REF_FIRST+(IRRef)(n+1u+shift);
  IRRef s_n = REF_FIRST+(IRRef)(2u*n+2u+shift);
  IRRef gt = s_n+1u;
  IRRef loop = s_n+2u;
  IRRef xpoll = s_n+3u;
  IRRef body_i = s_n+4u;
  IRRef lt = REF_FIRST+(IRRef)(3u*n+7u+shift);
  IRRef first_phi = lt+1u;
  IRRef ref;
  MSize j;
  SnapNo snapno;

  assert(ir[REF_BASE].o == IR_BASE && ir[REF_BASE].t.irt == IRT_PGC);
  assert(ir[REF_BASE].s == SPS_NONE);
  expect_ir_ins(ir, s_i, IR_SLOAD, IRT_INT|IRT_GUARD,
	(IRRef)(n+3u+shift), IRSLOAD_TYPECHECK);
  if (spec->ninvariant) {
    expect_ir_ins(ir, REF_FIRST+1u, IR_SLOAD, IRT_INT|IRT_GUARD,
	  3, IRSLOAD_TYPECHECK);
    s_k = REF_FIRST+2u;
    expect_ir_ins(ir, s_k, IR_SLOAD, IRT_INT|IRT_GUARD,
	  (IRRef)(n+3u), IRSLOAD_TYPECHECK);
    for (j = 2; j <= n; j++)
      expect_ir_ins(ir, REF_FIRST+(IRRef)(j+1u), IR_SLOAD,
	IRT_INT|IRT_GUARD, (IRRef)(j+2u), IRSLOAD_TYPECHECK);
  } else {
    for (j = 1; j <= n; j++)
      expect_ir_ins(ir, REF_FIRST+(IRRef)j, IR_SLOAD,
	IRT_INT|IRT_GUARD, (IRRef)(j+2u), IRSLOAD_TYPECHECK);
  }
  expect_ir_ins(ir, left_i, IR_ADDOV,
	IRT_INT|IRT_GUARD|IRT_ISPHI, s_i, find_kint(root->T, 1));
  for (j = 1; j <= n; j++) {
    IRRef s_a = spec->ninvariant ?
	(j == 1 ? REF_FIRST+1u : REF_FIRST+(IRRef)(j+1u)) :
	REF_FIRST+(IRRef)j;
    IRRef left_a = left_i+(IRRef)j;
    IRRef rhs = spec->ninvariant && j == 1 ? s_k :
	find_kint(root->T, (int32_t)j);
    if (spec->ninvariant && j == 1)
      expect_ir_ins(ir, left_a, IR_ADDOV,
	IRT_INT|IRT_GUARD|IRT_ISPHI, rhs, s_a);
    else
      expect_ir_ins(ir, left_a, IR_ADDOV,
	IRT_INT|IRT_GUARD|IRT_ISPHI, s_a, rhs);
  }
  expect_ir_ins(ir, s_n, IR_SLOAD, IRT_INT|IRT_GUARD,
	2, IRSLOAD_TYPECHECK);
  expect_ir_ins(ir, gt, IR_GT, IRT_INT|IRT_GUARD, s_n, left_i);
  expect_ir_ins(ir, loop, IR_LOOP, IRT_NIL|IRT_GUARD, 0, 0);
  expect_ir_ins(ir, xpoll, IR_XPOLL, IRT_NIL|IRT_GUARD, 1, 0);
  expect_ir_ins(ir, body_i, IR_ADDOV,
	IRT_INT|IRT_GUARD|IRT_ISPHI, left_i, find_kint(root->T, 1));
  for (j = 1; j <= n; j++) {
    IRRef rhs = spec->ninvariant && j == 1 ? s_k :
	find_kint(root->T, (int32_t)j);
    expect_ir_ins(ir, body_i+(IRRef)j, IR_ADDOV,
	IRT_INT|IRT_GUARD|IRT_ISPHI, left_i+(IRRef)j, rhs);
  }
  expect_ir_ins(ir, lt, IR_LT, IRT_INT|IRT_GUARD, body_i, s_n);
  for (j = 0; j <= n; j++)
    expect_ir_ins(ir, first_phi+(IRRef)j, IR_PHI, IRT_INT,
	left_i+(IRRef)j, body_i+(IRRef)j);
  assert(first_phi+(IRRef)(n+1u) == semantic_end);

  assert(trace_nsnap_acq(root->T) == spec->nsnap);
  for (snapno = 0; snapno < trace_nsnap_acq(root->T); snapno++)
    assert(snap_ref_acq(&trace_snap_acq(root->T)[snapno]) ==
	   expected_snapshot_ref(spec, snapno));
  assert(snap_ref_acq(&trace_snap_acq(root->T)[n+4u]) == loop);

  for (ref = semantic_end; ref < trace_nins_acq(root->T); ref++) {
    if (spec->suffix_nins == 1) {
      assert(ref == semantic_end && ir[ref].o == IR_NOP);
      assert(ir[ref].t.irt == IRT_NIL && ir[ref].op1 == 0 &&
	     ir[ref].op2 == 0 && ir[ref].prev == 0);
    } else {
      assert(ir[ref].o == IR_RENAME && ir[ref].t.irt == IRT_NIL);
      assert(ir[ref].op1 >= REF_FIRST && ir[ref].op1 < semantic_end);
      assert(ir[ref].op2 == n+4u && ir[ref].r < RID_MAX_GPR);
      assert(rset_test(RSET_GPR, ir[ref].r) && ir[ref].s == SPS_NONE);
    }
  }
  assert(trace_nins_acq(root->T) == semantic_end+spec->suffix_nins);
  expect_rename_suffix(root, semantic_end);
}

static void expect_spill_inventory(const SpillRoot *root)
{
  const SpillSpec *spec = root->spec;
  const IRIns *ir = trace_ir_acq(root->T);
  IRRef semantic_end = expected_semantic_end(spec);
  IRRef ref;
  MSize highest = 0, count = 0;
  for (ref = REF_BASE; ref < semantic_end; ref++) {
    if (ra_hasspill(ir[ref].s)) {
      assert(ir[ref].s >= SPS_FIRST && ir[ref].s <= spec->highest_slot);
      if (ir[ref].s > highest) highest = ir[ref].s;
      count++;
    }
  }
  assert(highest == spec->highest_slot);

  if (spec->kind == SPILL_FIXED) {
    assert(count == 2);
    assert(ir[REF_FIRST+FIXED_N+1u].s == 3);
    assert(ir[REF_FIRST+2u*FIXED_N+2u].s == 2);
  } else if (spec->kind == SPILL_MIN_DYNAMIC) {
    assert(count == 4);
    assert(ir[REF_FIRST].s == 5);
    assert(ir[REF_FIRST+2u].s == 2);
    assert(ir[REF_FIRST+MIN_DYNAMIC_N+2u].s == 3);
    assert(ir[REF_FIRST+2u*MIN_DYNAMIC_N+3u].s == 4);
  } else {
    IRRef left_i = REF_FIRST+HEAVY_N+1u;
    IRRef body_i = REF_FIRST+2u*HEAVY_N+6u;
    IRRef first_phi = REF_FIRST+3u*HEAVY_N+8u;
    assert(spec->kind == SPILL_HEAVY && count == 36);
    assert(ir[REF_FIRST].s == 35);
    assert(ir[REF_FIRST+1u].s == 34);
    assert(ir[REF_FIRST+2u].s == 33);
    assert(ir[left_i].s == 32);
    assert(ir[left_i+1u].s == 31);
    assert(ir[left_i+2u].s == 29);
    assert(ir[REF_FIRST+2u*HEAVY_N+2u].s == 30);
    assert(ir[body_i].s == 3);
    assert(ir[body_i+1u].s == 2);
    assert(ir[body_i+2u].s == 4);
    assert(ir[first_phi].s == 3 && ir[first_phi].op2 == body_i);
    assert(ir[first_phi+1u].s == 2 &&
	   ir[first_phi+1u].op2 == body_i+1u);
    assert(ir[first_phi+2u].s == SPS_NONE &&
	   ir[first_phi+2u].op2 == body_i+2u);
    /* These unequal left/right spill pairs force asm_phi_copyspill(). */
    assert(ir[ir[first_phi].op1].s == 32 &&
	   ir[ir[first_phi].op2].s == 3);
    assert(ir[ir[first_phi+1u].op1].s == 31 &&
	   ir[ir[first_phi+1u].op2].s == 2);
  }
}

static void expect_snapshot_tuple(const SpillRoot *root, SnapNo snapno,
	BCReg slot, IRRef ref, Reg reg, MSize spill)
{
  const GCtrace *T = root->T;
  const SnapShot *snap = &trace_snap_acq(T)[snapno];
  const SnapEntry *map = &trace_snapmap_acq(T)[snap_mapofs_acq(snap)];
  MSize n;
  assert(snapno < trace_nsnap_acq(T));
  for (n = 0; n < snap_nent_acq(snap); n++) {
    SnapEntry sn = snapentry_acq(&map[n]);
    if (!(sn & SNAP_FRAME) && snap_slot(sn) == slot) {
      RegSP rs;
      assert(snap_ref(sn) == ref);
      rs = effective_snapshot_regsp(T,
	expected_semantic_end(root->spec), snapno, ref);
      assert(regsp_reg(rs) == reg);
      assert(regsp_spill(rs) == spill);
      return;
    }
  }
  assert(!"missing effective snapshot tuple");
}

static void expect_critical_snapshot_tuples(const SpillRoot *root)
{
  if (root->spec->kind == SPILL_FIXED) {
    IRRef left_i = REF_FIRST+FIXED_N+1u;
    expect_snapshot_tuple(root, 2, 26, left_i, RID_X2, 3);
    /* The loop-snapshot RENAME changes the same historical value to X3. */
    expect_snapshot_tuple(root, FIXED_N+4u, 26, left_i, RID_X3, 0);
  } else if (root->spec->kind == SPILL_MIN_DYNAMIC) {
    IRRef left_i = REF_FIRST+MIN_DYNAMIC_N+2u;
    IRRef s_k = REF_FIRST+2u;
    expect_snapshot_tuple(root, 2, 27, left_i, RID_X2, 3);
    expect_snapshot_tuple(root, 2, 29, s_k, RID_X3, 2);
    expect_snapshot_tuple(root, MIN_DYNAMIC_N+4u, 27,
	left_i, RID_X2, 3);
    expect_snapshot_tuple(root, MIN_DYNAMIC_N+6u, 29,
	s_k, RID_X3, 2);
  } else {
    IRRef left_a2 = REF_FIRST+HEAVY_N+3u;
    IRRef body_a2 = REF_FIRST+2u*HEAVY_N+8u;
    assert(root->spec->kind == SPILL_HEAVY);
    /* Exit 33 is the slot-4 body ADDOV guard: restoration uses the
    ** pre-operation left value in copy-spill slot 29. The next snapshot
    ** observes the successful body result in slot 4. */
    expect_snapshot_tuple(root, HEAVY_N+7u, 4,
	left_a2, RID_X2, 29);
    expect_snapshot_tuple(root, HEAVY_N+8u, 4,
	body_a2, RID_X1, 4);
  }
}

static int32_t sign_extend_branch(uint32_t value, unsigned bits)
{
  return (int32_t)(value << (32u-bits)) >> (32u-bits);
}

static void expect_minimal_dynamic_prologue(const SpillRoot *root)
{
  const MCode *mcode = trace_mcode_acq(root->T);
  MSize nword = trace_szmcode_acq(root->T) / sizeof(MCode);
  MSize loopword = trace_mcloop_acq(root->T) / sizeof(MCode);
  MSize i, subword = (MSize)~0u;
  unsigned nsub = 0, nbackedge = 0;
  assert(root->spec->kind == SPILL_MIN_DYNAMIC);
  assert((trace_szmcode_acq(root->T) & (sizeof(MCode)-1u)) == 0);
  assert((trace_mcloop_acq(root->T) & (sizeof(MCode)-1u)) == 0);
  for (i = 0; i < nword; i++) {
    uint32_t ins = mcode[i];
    if (ins == UINT32_C(0xd10043ff)) {  /* SUB SP, SP, #16. */
      subword = i;
      nsub++;
    }
    if ((ins & UINT32_C(0xff000010)) == UINT32_C(0x54000000)) {
      int32_t delta = sign_extend_branch((ins >> 5) & 0x7ffffu, 19);
      if ((int64_t)i+delta == (int64_t)loopword) {
	assert((ins & 15u) == CC_LT);
	assert(i > loopword);
	nbackedge++;
      }
    }
  }
  assert(nsub == 1);
#if LJ_ABI_BRANCH_TRACK
  assert(mcode[0] == A64I_BTI_J && subword == 1);
#else
  assert(subword == 0);
#endif
  assert(loopword > subword+1u);
  assert(nbackedge == 1);
}

static void expect_no_other_traces(const SpillRoot *root)
{
  TraceNo traceno;
  for (traceno = 2; (MSize)traceno < trace_sizetrace_acq(root->J); traceno++)
    assert(!trace_runnable_acq(traceref_safe(root->J, traceno), traceno));
}

static void expect_trace_identity(SpillRoot *root)
{
  GCtrace *T = traceref_safe(root->J, 1);
  const BCIns *pc;
  BCIns patched;
  MSize ntuples;
  uint64_t irhash, snaphash;

  assert(T != NULL && trace_runnable_acq(T, 1));
  assert(trace_traceno_acq(T) == 1 && trace_root_acq(T) == 0);
  assert(trace_link_acq(T) == 1 && trace_linktype_acq(T) == LJ_TRLINK_LOOP);
  assert(trace_nchild_acq(T) == 0 && trace_nextside_acq(T) == 0);
  assert(trace_startpt_acq(T) == root->pt);
  assert(trace_topslot_acq(T) == (MSize)root->pt->framesize);
  assert(trace_spadjust_acq(T) == root->spec->spadjust);
  assert((la_load8_acq(&T->unused1) & TRACE_ARM64_INT_LOOP_ADMITTED) != 0);
  assert(trace_mcode_acq(T) != NULL && trace_szmcode_acq(T) > sizeof(MCode));
  assert(trace_mcloop_acq(T) > 0 && trace_mcloop_acq(T) <
	 trace_szmcode_acq(T));
  pc = trace_startpc_acq(T);
  assert(pc != NULL && bc_op(trace_startins_acq(T)) == BC_LOOP);
  patched = (BCIns)la_load32_acq((const uint32_t *)pc);
  assert(bc_op(patched) == BC_JLOOP && bc_d(patched) == 1);
  assert(proto_trace_acq(root->pt) == 1);

  root->T = T;
  root->loopref = expected_snapshot_ref(root->spec,
	(SnapNo)(root->spec->nvalue+4u));
  root->loopsnap = (SnapNo)(root->spec->nvalue+4u);
  expect_semantics(root);
  expect_spill_inventory(root);
  expect_critical_snapshot_tuples(root);
  irhash = ir_fingerprint(T);
  snaphash = snapshot_fingerprint(T, expected_semantic_end(root->spec),
	&ntuples);
  assert(trace_nsnapmap_acq(T) == root->spec->nsnapmap);
  assert(ntuples == root->spec->snapshot_tuples);
  assert(irhash == root->spec->ir_fingerprint);
  assert(snaphash == root->spec->snapshot_fingerprint);
  if (root->spec->kind == SPILL_MIN_DYNAMIC)
    expect_minimal_dynamic_prologue(root);
  expect_no_other_traces(root);
}

static SpillRoot spill_root_new(const SpillSpec *spec)
{
  SpillRoot root;
  char source[SOURCE_CAPACITY];
  int first;
  memset(&root, 0, sizeof(root));
  memset(source, 0, sizeof(source));
  root.L = luaL_newstate();
  assert(root.L != NULL);
  luaL_openlibs(root.L);
  root.g = G(root.L);
  root.J = L2J(root.L);
  root.tg = L2TG(root.L);
  root.spec = spec;
  root.idle_vmstate = lj_tg_vmstate_load_acq(root.tg);
  build_chunk(spec, source, sizeof(source));
  run_lua(root.L, source);
  root.pt = spill_proto(root.L);
  first = call_vector(root.L, spec, 20, 0, 0);
  expect_vector(root.L, spec, first, 20, 0);
  expect_trace_identity(&root);
  assert(lj_tg_load_jit_base(root.tg) == NULL);
  assert(lj_tg_in_native_acq(root.tg) == 0);
  assert(lj_tg_vmstate_load_acq(root.tg) == root.idle_vmstate);
  return root;
}

static void spill_root_close(SpillRoot *root)
{
  expect_no_other_traces(root);
  assert(lj_tg_load_jit_base(root->tg) == NULL);
  assert(lj_tg_in_native_acq(root->tg) == 0);
  assert(lj_tg_vmstate_load_acq(root->tg) == root->idle_vmstate);
  lua_close(root->L);
  memset(root, 0, sizeof(*root));
}

static void expect_quiescent(const SpillRoot *root)
{
  assert(lj_tg_load_jit_base(root->tg) == NULL);
  assert(lj_tg_in_native_acq(root->tg) == 0);
  assert(lj_tg_vmstate_load_acq(root->tg) == root->idle_vmstate);
  expect_no_other_traces(root);
}

static void expect_one_exit(SnapNo exitno)
{
  assert(lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() == 1);
  assert(lj_trace_test_first_exit_parent() == 1);
  assert(lj_trace_test_first_exitno() == exitno);
  assert(lj_trace_test_last_exit_parent() == 1);
  assert(lj_trace_test_last_exitno() == exitno);
}

static void call_and_expect_native_vector(SpillRoot *root, int negative)
{
  int first;
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  first = call_vector(root->L, root->spec, 20, negative, 0);
  expect_vector(root->L, root->spec, first, 20, negative);
  expect_one_exit((SnapNo)(root->spec->nsnap-1u));
  expect_quiescent(root);
}

typedef enum PostAdmissionRequest {
  POSTADMISSION_PROFILE,
  POSTADMISSION_STOPREQ
} PostAdmissionRequest;

typedef struct PostAdmissionPublisher {
  global_State *g;
  TGState *tg;
  uint64_t epoch;
  PostAdmissionRequest request;
  uint32_t saw_stage;
  uint32_t saw_jit_base;
  uint32_t published;
} PostAdmissionPublisher;

static void clear_stopreq(TGState *tg)
{
  (void)lj_tg_flags_and_rlx(tg,
	(uint8_t)~(TGF_STOPREQ|TGF_STOPREQ_FRESH));
}

static void *publish_postadmission_request(void *arg)
{
  PostAdmissionPublisher *publisher = (PostAdmissionPublisher *)arg;
  uint32_t i;
  for (i = 0; i < 10000000u; i++) {
    if (lj_trace_test_root_entry_paused() ==
	LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION) {
      la_store32_rel(&publisher->saw_stage, 1);
      break;
    }
    (void)lj_thr_retry_yield(NULL);
  }
  assert(la_load32_acq(&publisher->saw_stage) == 1);
  assert(gc2_hs_epoch_acq(publisher->g) == publisher->epoch);
  assert(lj_tg_hs_epoch_ack_acq(publisher->tg) == publisher->epoch);
  assert(gc2_hs_pending_acq(publisher->g) == 0);
  assert(lj_tg_reqmask_acq(publisher->tg) == 0);
  assert(lj_tg_poll_acq(publisher->tg) == 0);
  if (lj_tg_load_jit_base(publisher->tg) != NULL)
    la_store32_rel(&publisher->saw_jit_base, 1);
  assert(la_load32_acq(&publisher->saw_jit_base) == 1);
  if (publisher->request == POSTADMISSION_PROFILE) {
    lj_tg_profile_request_rel(publisher->tg, 1);
  } else {
    assert(publisher->request == POSTADMISSION_STOPREQ);
    gc2_hs_actions_rel(publisher->g, LJ_GC2_HS_STOPREQ);
    gc2_hs_pending_rel(publisher->g, 1);
    gc2_hs_epoch_rel(publisher->g, publisher->epoch+1u);
    lj_tg_reqmask_rel(publisher->tg, LJ_GC2_HS_STOPREQ);
    lj_tg_poll_rel(publisher->tg, 1);
  }
  la_store32_rel(&publisher->published, 1);
  lj_trace_test_root_entry_release();
  return NULL;
}

static void test_fixed_vectors(void)
{
  SpillRoot root = spill_root_new(&fixed_spec);
  call_and_expect_native_vector(&root, 0);
  call_and_expect_native_vector(&root, 1);
  spill_root_close(&root);
}

static void test_minimal_dynamic_lifecycle(void)
{
  SpillRoot root = spill_root_new(&min_dynamic_spec);
  uint64_t epoch = gc2_hs_epoch_acq(root.g);
  PostAdmissionPublisher publisher;
  pthread_t worker;
  int first, base, nargs, nresults, status;
  void *saved_cframe;

  call_and_expect_native_vector(&root, 0);
  call_and_expect_native_vector(&root, 1);

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
  publisher = (PostAdmissionPublisher){
    root.g, root.tg, epoch, POSTADMISSION_PROFILE, 0, 0, 0
  };
  assert(pthread_create(&worker, NULL, publish_postadmission_request,
	&publisher) == 0);
  first = call_vector(root.L, root.spec, 20, 0, 0);
  expect_vector(root.L, root.spec, first, 20, 0);
  assert(pthread_join(worker, NULL) == 0);
  assert(la_load32_acq(&publisher.published) == 1);
  assert(lj_trace_test_root_entry_publishes() == 2);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() == 2);
  assert(lj_trace_test_first_exit_parent() == 1);
  assert(lj_trace_test_first_exitno() == root.loopsnap);
  assert(lj_trace_test_last_exit_parent() == 1);
  assert(lj_trace_test_last_exitno() == root.spec->nsnap-1u);
  assert(gc2_hs_epoch_acq(root.g) == epoch);
  assert(lj_tg_profile_request_acq(root.tg) == 0);
  expect_quiescent(&root);

  clear_stopreq(root.tg);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
  publisher = (PostAdmissionPublisher){
    root.g, root.tg, epoch, POSTADMISSION_STOPREQ, 0, 0, 0
  };
  assert(pthread_create(&worker, NULL, publish_postadmission_request,
	&publisher) == 0);
  base = lua_gettop(root.L);
  saved_cframe = root.L->cframe;
  nargs = 1+(int)root.spec->nvalue+(int)root.spec->ninvariant;
  nresults = nargs;
  assert(lua_checkstack(root.L, nargs+nresults+8));
  lua_getglobal(root.L, "__arm64_spill_loop");
  push_vector(root.L, root.spec, 20, 0, 0);
  status = lua_pcall(root.L, nargs, nresults, 0);
  assert(pthread_join(worker, NULL) == 0);
  assert(status == LUA_ERRRUN);
  assert(root.L->cframe == saved_cframe);
  assert(lua_isstring(root.L, -1));
  assert(strstr(lua_tostring(root.L, -1),
	"thread interrupted: VM shutdown") != NULL);
  lua_settop(root.L, base);
  assert(la_load32_acq(&publisher.published) == 1);
  expect_one_exit(root.loopsnap);
  assert(gc2_hs_epoch_acq(root.g) == epoch+1u);
  assert(lj_tg_hs_epoch_ack_acq(root.tg) == epoch+1u);
  assert(gc2_hs_pending_acq(root.g) == 0);
  assert(lj_tg_reqmask_acq(root.tg) == 0);
  assert(lj_tg_poll_acq(root.tg) == 0);
  assert((lj_tg_flags_acq(root.tg) & TGF_STOPREQ) != 0);
  expect_quiescent(&root);
  clear_stopreq(root.tg);

  call_and_expect_native_vector(&root, 0);
  spill_root_close(&root);
}

static void expect_heavy_overflow_results(SpillRoot *root, int first)
{
  MSize j;
  TValue *a2 = root->L->base+(first+2)-1;
  assert(lua_tointeger(root->L, first) == 2);
  for (j = 1; j <= root->spec->nvalue; j++) {
    if (j == 2) {
      assert(lua_tonumber(root->L, first+(int)j) == 2147483648.0);
    } else {
      assert(lua_tointeger(root->L, first+(int)j) ==
	     1000+(lua_Integer)j+2*(lua_Integer)j);
    }
  }
  assert(tvisnum(a2));
  assert(numV(a2) == 2147483648.0);
  lua_settop(root->L, first-1);
}

static void test_heavy_copyspill_overflow(void)
{
  SpillRoot root = spill_root_new(&heavy_spec);
  int first;
  call_and_expect_native_vector(&root, 0);
  call_and_expect_native_vector(&root, 1);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  first = call_vector(root.L, root.spec, 2, 0, 1);
  expect_heavy_overflow_results(&root, first);
  expect_one_exit(HEAVY_N+7u);
  expect_quiescent(&root);
  spill_root_close(&root);
}

int main(void)
{
  test_fixed_vectors();
  test_minimal_dynamic_lifecycle();
  test_heavy_copyspill_overflow();
  puts("t-arm64-jit-integer-spills OK");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-integer-spills SKIP");
  return 0;
}

#endif
