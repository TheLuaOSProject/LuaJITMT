/*
** Direct C contract for the strict ARM64 root-entry helper and VM replay.
** A real-prototype-backed synthetic trace reaches direct helper success, but
** its inert machine-code target is never executed here.
*/

#include <assert.h>
#include <stddef.h>
#include <pthread.h>
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
#include "lj_buf.h"
#include "lj_dispatch.h"
#include "lj_func.h"
#include "lj_gc2.h"
#include "lj_jit.h"
#include "lj_asm.h"
#include "lj_target.h"
#include "lj_thr.h"
#include "lj_tg.h"
#include "lj_trace.h"

#if !LJ_HASJIT || LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_FORL_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED || \
    LJ_ARM64_JIT_FORL_NATIVE_ENTRY_FAIL_CLOSED || \
    LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED
#error "t-arm64-jit-root-entry requires open LOOP/FORL root entry gates"
#endif

typedef enum RootEntryRaceMode {
  ROOT_ENTRY_CLOSER_BEFORE_PUBLISH,
  ROOT_ENTRY_CLOSER_AFTER_PUBLISH,
  ROOT_ENTRY_REQUEST_AFTER_PUBLISH,
  ROOT_ENTRY_POLL_AFTER_METADATA,
  ROOT_ENTRY_REQMASK_AFTER_METADATA,
  ROOT_ENTRY_PROFILE_AFTER_METADATA
} RootEntryRaceMode;

typedef struct RootEntryRace {
  global_State *g;
  TGState *tg;
  RootEntryRaceMode mode;
  uint32_t entry_done;
  uint32_t worker_done;
  uint32_t saw_active;
  int entered;
} RootEntryRace;

typedef struct RootEntryPatch {
  BCIns *pc;
  BCIns original;
} RootEntryPatch;

typedef struct RootEntryNumericPatch {
  RootEntryPatch forl;
  BCIns *fori_pc;
  BCIns fori_original;
} RootEntryNumericPatch;

typedef struct RootEntryTraceVec2 {
  MSize sizetrace;
  uint64_t retire_epoch;
  TraceVec *retired_next;
  GCRef slot[2];
} RootEntryTraceVec2;

enum {
  ROOT_ENTRY_R_VALUE = REF_FIRST,
  ROOT_ENTRY_R_LOOP,
  ROOT_ENTRY_R_SUFFIX,
  ROOT_ENTRY_R_END,
  ROOT_ENTRY_IR_CAP = ROOT_ENTRY_R_END
};

typedef struct RootEntryMetadataFixture {
  GCtrace saved_cur;
  TraceVec *saved_tracev;
  MSize saved_sizetrace;
  RootEntryTraceVec2 tracev;
  IRIns ir[ROOT_ENTRY_IR_CAP];
  SnapShot snap[1];
  SnapEntry snapmap[3];
  MCode mcode[4];
} RootEntryMetadataFixture;

typedef struct RootEntryFrameFixture {
  TValue saved_func;
} RootEntryFrameFixture;

static void run_lua(lua_State *L, const char *chunk)
{
  if (luaL_dostring(L, chunk) != 0) {
    fprintf(stderr, "root-entry Lua setup failed: %s\n",
	    lua_tostring(L, -1));
    assert(0);
  }
}

static GCfunc *global_lfunc(lua_State *L, const char *name)
{
  GCfunc *fn;
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  fn = funcV(L->top - 1);
  assert(isluafunc(fn));
  lua_pop(L, 1);
  return fn;
}

static GCproto *global_proto(lua_State *L, const char *name)
{
  return funcproto(global_lfunc(L, name));
}

static RootEntryPatch patch_first_root(lua_State *L, const char *name,
				       BCOp originalop, BCOp jitop)
{
  GCproto *pt = global_proto(L, name);
  BCIns *bc = proto_bc(pt);
  BCPos i;
  RootEntryPatch patch = { NULL, 0 };
  for (i = 0; i < pt->sizebc; i++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[i]);
    if (bc_op(ins) == originalop) {
      patch.pc = &bc[i];
      patch.original = ins;
      proto_jit_startins_rel(pt, patch.pc, ins);
      bc_publish(patch.pc, BCINS_AD(jitop, bc_a(ins), 1));
      return patch;
    }
  }
  assert(!"root-entry fixture opcode not found");
  return patch;
}

static void restore_root_patch(RootEntryPatch *patch)
{
  assert(patch != NULL && patch->pc != NULL);
  bc_publish(patch->pc, patch->original);
}

static RootEntryNumericPatch patch_numeric_root(lua_State *L,
						 const char *name, int patch_fori)
{
  RootEntryNumericPatch patch;
  patch.forl = patch_first_root(L, name, BC_FORL, BC_JFORL);
  patch.fori_pc = patch.forl.pc + bc_j(patch.forl.original);
  patch.fori_original = (BCIns)la_load32_acq(
    (const uint32_t *)patch.fori_pc);
  assert(bc_op(patch.fori_original) == BC_FORI);
  if (patch_fori) {
    bc_publish(patch.fori_pc,
      BCINS_AD(BC_JFORI, bc_a(patch.fori_original),
	       bc_d(patch.fori_original)));
  } else {
    patch.fori_pc = NULL;
  }
  return patch;
}

static void restore_numeric_patch(RootEntryNumericPatch *patch)
{
  if (patch->fori_pc != NULL)
    bc_publish(patch->fori_pc, patch->fori_original);
  restore_root_patch(&patch->forl);
}

static void install_root_entry_frame(lua_State *L, GCfunc *fn,
				     RootEntryFrameFixture *fixture)
{
  assert(fn != NULL && isluafunc(fn));
  copyTV(L, &fixture->saved_func, L->base-2);
  setfuncV(L, L->base-2, fn);
  assert(curr_func(L) == fn);
}

static void remove_root_entry_frame(lua_State *L,
				    const RootEntryFrameFixture *fixture)
{
  copyTV(L, L->base-2, &fixture->saved_func);
}

static void root_entry_setir(IRIns *ir, IRRef ref, IROp op, IRType type,
	IRRef op1, IRRef op2, Reg reg, MSize spill)
{
  assert(ref < ROOT_ENTRY_IR_CAP && reg <= UINT8_MAX && spill <= UINT8_MAX);
  memset(&ir[ref], 0, sizeof(ir[ref]));
  ir[ref].op1 = (IRRef1)op1;
  ir[ref].op2 = (IRRef1)op2;
  ir[ref].t.irt = (uint8_t)type;
  ir[ref].o = (IROp1)op;
  ir[ref].r = (uint8_t)reg;
  ir[ref].s = (uint8_t)spill;
}

static void install_root_entry_metadata(jit_State *J, GCproto *pt,
					const RootEntryPatch *loop,
					RootEntryMetadataFixture *fixture)
{
  uint64_t snapshot_pcbase;
  memset(fixture, 0, sizeof(*fixture));
  fixture->saved_cur = J->cur;
  fixture->saved_tracev = tracevec_acq(J);
  fixture->saved_sizetrace = trace_sizetrace_acq(J);
  assert(fixture->saved_tracev == NULL);
  assert(offsetof(RootEntryTraceVec2, slot) == offsetof(TraceVec, slot));

  memset(&J->cur, 0, sizeof(J->cur));
  J->cur.gct = (uint32_t)~LJ_TTRACE;
  trace_traceno_rel(&J->cur, 1);
  J->cur.root = 0;
  trace_link_rel(&J->cur, 1);
  J->cur.linktype = LJ_TRLINK_LOOP;
  trace_nextside_rel(&J->cur, 0);
  J->cur.nchild = 0;
  J->cur.spadjust = 0;
  J->cur.topslot = pt->framesize;
  trace_startpt_rel(&J->cur, pt);
  setmref(J->cur.startpc, loop->pc);
  J->cur.startins = loop->original;
  J->cur.ir = fixture->ir;
  J->cur.nins = ROOT_ENTRY_R_END;
  J->cur.nk = REF_TRUE;
  J->cur.snap = fixture->snap;
  J->cur.snapmap = fixture->snapmap;
  J->cur.nsnap = 1;
  J->cur.nsnapmap = 3;
  J->cur.unused1 = TRACE_ARM64_INT_LOOP_ADMITTED;

  root_entry_setir(fixture->ir, REF_BASE, IR_BASE, IRT_PGC,
		   0, 0, RID_X0, SPS_NONE);
  root_entry_setir(fixture->ir, ROOT_ENTRY_R_VALUE, IR_SLOAD,
		   IRT_INT|IRT_GUARD, 2, IRSLOAD_TYPECHECK,
		   RID_X0, SPS_NONE);
  root_entry_setir(fixture->ir, ROOT_ENTRY_R_LOOP, IR_LOOP,
		   IRT_NIL|IRT_GUARD, 0, 0, RID_X0, SPS_NONE);
  root_entry_setir(fixture->ir, ROOT_ENTRY_R_SUFFIX, IR_NOP, IRT_NIL,
		   0, 0, RID_X0, SPS_NONE);
  fixture->snap[0].ref = ROOT_ENTRY_R_LOOP;
  fixture->snap[0].mapofs = 0;
  assert(pt->framesize > 2);
  fixture->snap[0].nslots = pt->framesize;
  fixture->snap[0].topslot = pt->framesize;
  fixture->snap[0].nent = 1;
  fixture->snapmap[0] = SNAP(2, 0, ROOT_ENTRY_R_VALUE);
  snapshot_pcbase = (uint64_t)(uintptr_t)loop->pc << 8;
  memcpy(&fixture->snapmap[1], &snapshot_pcbase, sizeof(snapshot_pcbase));

  fixture->mcode[0] = 0xd503201fu;  /* Unreachable AArch64 NOPs. */
  fixture->mcode[1] = 0xd503201fu;
  fixture->mcode[2] = 0xd503201fu;
  fixture->mcode[3] = 0xd503201fu;
  J->cur.szmcode = (MSize)sizeof(fixture->mcode);
  J->cur.mcode = fixture->mcode;
  J->cur.mcloop = (MSize)sizeof(MCode);
#if LJ_ABI_PAUTH
  J->cur.mcauth = lj_ptr_sign((ASMFunction)(void *)fixture->mcode, &J->cur);
#endif

  fixture->tracev.sizetrace = 2;
  fixture->tracev.retire_epoch = 0;
  fixture->tracev.retired_next = NULL;
  setgcrefrel(fixture->tracev.slot[0], NULL);
  setgcrefrel(fixture->tracev.slot[1], obj2gco(&J->cur));
  trace_sizetrace_rel(J, 2);
  tracevec_rel(J, (TraceVec *)&fixture->tracev);
}

static void remove_root_entry_metadata(jit_State *J,
				       RootEntryMetadataFixture *fixture)
{
  tracevec_rel(J, fixture->saved_tracev);
  trace_sizetrace_rel(J, fixture->saved_sizetrace);
  J->cur = fixture->saved_cur;
}

static void call_global(lua_State *L, const char *name, int nargs, int nresults)
{
  int base = lua_gettop(L) - nargs;
  lua_getglobal(L, name);
  lua_insert(L, base + 1);
  if (lua_pcall(L, nargs, nresults, 0) != 0) {
    fprintf(stderr, "root-entry call %s failed: %s\n", name,
	    lua_tostring(L, -1));
    assert(0);
  }
}

/* Model the exact full instruction consumed by a VM caller. Tests which
** deliberately mutate the live word after this construction still exercise
** the helper's consumed-versus-current generation check. */
static LJTraceRootEntry test_trace_enter_root(jit_State *J, const BCIns *pc,
	TraceNo traceno, lua_State *L, TValue *base, BCOp sourceop)
{
  BCReg a = pc != NULL ? bc_a((BCIns)la_load32_acq(
	(const uint32_t *)pc)) : 0;
  BCIns sourceins = BCINS_AD(sourceop, a, traceno);
  return lj_trace_enter_root(J, pc, traceno, L, base, sourceins);
}

/* Kept out of line for the contract script: its call site proves that Clang's
** Darwin AAPCS64 lowering consumes LJTraceRootEntry.trace from x0 and target
** from x1, with no hidden result pointer. It is deliberately never executed. */
__attribute__((noinline, used))
GCtrace *lj_test_root_entry_abi_probe(jit_State *J, const BCIns *pc,
	TraceNo traceno, lua_State *L, TValue *base, BCIns sourceins,
	ASMFunction *targetp)
{
  LJTraceRootEntry entry =
    lj_trace_enter_root(J, pc, traceno, L, base, sourceins);
  *targetp = entry.target;
  return entry.trace;
}

static void expect_reject(LJTraceRootEntry entry)
{
  assert(entry.trace == NULL);
  assert(entry.target == NULL);
}

static int root_entry_metadata_layout_valid(const GCtrace *T)
{
  LJArm64PostRAView view;
  GCproto *pt = trace_startpt_acq((GCtrace *)T);
  IRRef semantic_nins = 0;
  int ok;
  view.ir = T->ir;
  view.snap = T->snap;
  view.snapmap = T->snapmap;
  view.proto_bc = proto_bc(pt);
  view.nins = T->nins;
  view.nk = T->nk;
  view.nsnap = T->nsnap;
  view.nsnapmap = T->nsnapmap;
  view.spadjust = T->spadjust;
  view.proto_sizebc = pt->sizebc;
  view.root_topslot = T->topslot;
  view.startins = T->startins;
  view.base_delta = 0;
  ok = lj_asm_arm64_postra_admit(&view, &semantic_nins);
  if (ok)
    assert(semantic_nins == ROOT_ENTRY_R_SUFFIX);
  return ok;
}

static void expect_metadata_reject(lua_State *L, const BCIns *pc)
{
  uint32_t publishes = lj_trace_test_root_entry_publishes();
  uint32_t cleanups = lj_trace_test_root_entry_cleanups();
  expect_reject(test_trace_enter_root(L2J(L), pc, 1, L, L->base, BC_JLOOP));
  assert(lj_tg_load_jit_base(L->tg_hint) == NULL);
  assert(lj_trace_test_root_entry_publishes() == publishes + 1u);
  assert(lj_trace_test_root_entry_cleanups() == cleanups + 1u);
}

static void expect_metadata_success(lua_State *L, const BCIns *pc,
				    RootEntryMetadataFixture *fixture)
{
  lua_State *saved_tmpbuf_L = sbufL(&L->tg_hint->tmpbuf);
  uint32_t publishes = lj_trace_test_root_entry_publishes();
  uint32_t cleanups = lj_trace_test_root_entry_cleanups();
  assert(root_entry_metadata_layout_valid(&L2J(L)->cur));
  LJTraceRootEntry entry = test_trace_enter_root(
    L2J(L), pc, 1, L, L->base, BC_JLOOP);
  assert(entry.trace == &L2J(L)->cur);
  assert(entry.target != NULL);
  assert((uintptr_t)lj_ptr_strip(entry.target) ==
	 (uintptr_t)(void *)fixture->mcode);
  assert(lj_tg_load_jit_base(L->tg_hint) == L->base);
  assert(lj_trace_test_root_entry_publishes() == publishes + 1u);
  assert(lj_trace_test_root_entry_cleanups() == cleanups);
  /* Direct C validation must not execute the inert target. Release its exact
  ** synthetic entry intent just as a native exit would. */
  lj_tg_store_jit_base(L->tg_hint, NULL);
  setsbufL(&L->tg_hint->tmpbuf, saved_tmpbuf_L);
}

static void expect_layout_reject(lua_State *L, const BCIns *pc)
{
  assert(!root_entry_metadata_layout_valid(&L2J(L)->cur));
  expect_metadata_reject(L, pc);
}

static void test_spill_layout_mutations(lua_State *L, const BCIns *pc,
					RootEntryMetadataFixture *fixture)
{
  GCtrace *T = &L2J(L)->cur;
  IRIns *ir = T->ir;

  LJ_STATIC_ASSERT(SPS_FIRST == 2);
  LJ_STATIC_ASSERT(SPS_FIXED == 4);
  LJ_STATIC_ASSERT(SPS_LIMIT == 256);

  /* The interpreter's fixed 16-byte reserve covers slots 2 and 3 without a
  ** dynamic adjustment. Both the shared validator and the root-entry gate
  ** must accept those exact layouts. */
  ir[ROOT_ENTRY_R_VALUE].s = SPS_FIRST;
  T->spadjust = 0;
  expect_metadata_success(L, pc, fixture);
  ir[ROOT_ENTRY_R_VALUE].s = SPS_FIXED-1;
  expect_metadata_success(L, pc, fixture);

  /* Slot 4 is the first dynamic slot and requires one canonical 16-byte
  ** extension beyond the fixed VM reserve. */
  ir[ROOT_ENTRY_R_VALUE].s = SPS_FIXED;
  T->spadjust = 16;
  expect_metadata_success(L, pc, fixture);

  T->spadjust = 4;
  expect_layout_reject(L, pc);
  T->spadjust = 8;
  expect_layout_reject(L, pc);
  T->spadjust = 12;
  expect_layout_reject(L, pc);

  /* Aligned but non-canonical dynamic sizes fail in both directions. */
  T->spadjust = 0;
  expect_layout_reject(L, pc);
  T->spadjust = 32;
  expect_layout_reject(L, pc);

  assert((MSize)sps_scale(SPS_LIMIT-SPS_FIXED) == 1008u);
  T->spadjust = (MSize)sps_scale(SPS_LIMIT-SPS_FIXED) + 16u;
  expect_layout_reject(L, pc);

  /* The advertised 16 bytes provide slots 4..7, never slot 8. */
  ir[ROOT_ENTRY_R_VALUE].s = SPS_FIXED+4;
  T->spadjust = 16;
  expect_layout_reject(L, pc);

  ir[ROOT_ENTRY_R_VALUE].s = SPS_FIRST-1;
  T->spadjust = 0;
  expect_layout_reject(L, pc);

  /* Structural and guard-only instructions never own spill storage. */
  ir[ROOT_ENTRY_R_VALUE].s = SPS_NONE;
  root_entry_setir(ir, REF_BASE, IR_BASE, IRT_PGC,
		   0, 0, RID_X0, SPS_FIRST);
  expect_layout_reject(L, pc);
  root_entry_setir(ir, REF_BASE, IR_BASE, IRT_PGC,
		   0, 0, RID_X0, SPS_NONE);

  root_entry_setir(ir, ROOT_ENTRY_R_VALUE, IR_LT,
		   IRT_INT|IRT_GUARD, REF_NIL, REF_TRUE,
		   RID_X0, SPS_FIRST);
  expect_layout_reject(L, pc);
  root_entry_setir(ir, ROOT_ENTRY_R_VALUE, IR_LOOP,
		   IRT_NIL|IRT_GUARD, 0, 0, RID_X0, SPS_FIRST);
  expect_layout_reject(L, pc);
  root_entry_setir(ir, ROOT_ENTRY_R_VALUE, IR_XPOLL,
		   IRT_NIL|IRT_GUARD, 1, 0, RID_X0, SPS_FIRST);
  expect_layout_reject(L, pc);

  /* Even an otherwise eligible producer is rejected when its spilled value
  ** type is not the exact checked integer family. */
  root_entry_setir(ir, ROOT_ENTRY_R_VALUE, IR_SLOAD,
		   IRT_NUM|IRT_GUARD, 2, IRSLOAD_TYPECHECK,
		   RID_X0, SPS_FIRST);
  expect_layout_reject(L, pc);
  root_entry_setir(ir, ROOT_ENTRY_R_VALUE, IR_SLOAD,
		   IRT_INT|IRT_GUARD, 2, IRSLOAD_TYPECHECK,
		   RID_X0, SPS_NONE);

  /* A terminal, register-only RENAME is valid and changes the effective
  ** snapshot location. Its own spill, source ref, snapshot number and
  ** register byte are independent fail-closed boundaries. */
  root_entry_setir(ir, ROOT_ENTRY_R_SUFFIX, IR_RENAME, IRT_NIL,
		   ROOT_ENTRY_R_VALUE, 0, RID_X1, SPS_NONE);
  expect_metadata_success(L, pc, fixture);

  ir[ROOT_ENTRY_R_SUFFIX].s = SPS_FIRST;
  expect_layout_reject(L, pc);
  root_entry_setir(ir, ROOT_ENTRY_R_SUFFIX, IR_RENAME, IRT_NIL,
		   ROOT_ENTRY_R_SUFFIX, 0, RID_X1, SPS_NONE);
  expect_layout_reject(L, pc);
  root_entry_setir(ir, ROOT_ENTRY_R_SUFFIX, IR_RENAME, IRT_NIL,
		   ROOT_ENTRY_R_VALUE, T->nsnap, RID_X1, SPS_NONE);
  expect_layout_reject(L, pc);
  root_entry_setir(ir, ROOT_ENTRY_R_SUFFIX, IR_RENAME, IRT_NIL,
		   ROOT_ENTRY_R_VALUE, 0, RID_MAX_GPR, SPS_NONE);
  expect_layout_reject(L, pc);

  root_entry_setir(ir, ROOT_ENTRY_R_SUFFIX, IR_NOP, IRT_NIL,
		   0, 0, RID_X0, SPS_NONE);
  T->spadjust = 0;
  assert(root_entry_metadata_layout_valid(T));
}

static void test_snapshot_layout_mutations(lua_State *L, const BCIns *pc)
{
  GCtrace *T = &L2J(L)->cur;
  SnapShot *snap = T->snap;
  SnapEntry *snapmap = T->snapmap;
  MSize saved_nslots = snap[0].nslots;
  MSize saved_topslot = snap[0].topslot;
  GCproto *pt = trace_startpt_acq(T);
  uint64_t saved_pcbase, bad_pcbase;

  assert(root_entry_metadata_layout_valid(T));

  /* The frozen map is one canonical partition: one semantic entry followed
  ** by the exact PC/base payload. In-bounds shifts and alternate spans fail. */
  snap[0].mapofs = 1;
  expect_layout_reject(L, pc);
  snap[0].mapofs = 0;
  snap[0].nent = 0;
  expect_layout_reject(L, pc);
  snap[0].nent = 1;
  snap[0].nslots = 2;
  expect_layout_reject(L, pc);
  snap[0].nslots = (uint8_t)saved_nslots;
  snap[0].nslots = (uint8_t)(T->topslot+2u+LJ_FR2);
  expect_layout_reject(L, pc);
  snap[0].nslots = (uint8_t)saved_nslots;
  snap[0].topslot = (uint8_t)(saved_topslot-1u);
  expect_layout_reject(L, pc);
  snap[0].topslot = (uint8_t)(saved_topslot+1u);
  expect_layout_reject(L, pc);
  snap[0].topslot = (uint8_t)saved_topslot;

  snapmap[0] = SNAP(2, SNAP_NORESTORE, ROOT_ENTRY_R_VALUE);
  expect_layout_reject(L, pc);
  snapmap[0] = SNAP(2, SNAP_FRAME, ROOT_ENTRY_R_VALUE);
  expect_layout_reject(L, pc);

  /* Exit restoration can only name an integer value that precedes this
  ** snapshot, or an exact KINT in the advertised constant interval. */
  snapmap[0] = SNAP(2, 0, ROOT_ENTRY_R_LOOP);
  expect_layout_reject(L, pc);
  snap[0].ref = ROOT_ENTRY_R_VALUE;
  snapmap[0] = SNAP(2, 0, ROOT_ENTRY_R_VALUE);
  expect_layout_reject(L, pc);
  snap[0].ref = ROOT_ENTRY_R_LOOP;
  snapmap[0] = SNAP(2, 0, REF_TRUE-1u);
  expect_layout_reject(L, pc);

  snapmap[0] = SNAP(2, 0, ROOT_ENTRY_R_VALUE);
  T->nk = 0;
  expect_layout_reject(L, pc);
  T->nk = REF_TRUE;

  memcpy(&saved_pcbase, &snapmap[1], sizeof(saved_pcbase));
  bad_pcbase = saved_pcbase | UINT64_C(1);
  memcpy(&snapmap[1], &bad_pcbase, sizeof(bad_pcbase));
  expect_layout_reject(L, pc);
  bad_pcbase = ((uint64_t)(uintptr_t)proto_bc(pt)+1u) << 8;
  memcpy(&snapmap[1], &bad_pcbase, sizeof(bad_pcbase));
  expect_layout_reject(L, pc);
  bad_pcbase = ((uint64_t)((uintptr_t)proto_bc(pt)-sizeof(BCIns))) << 8;
  memcpy(&snapmap[1], &bad_pcbase, sizeof(bad_pcbase));
  expect_layout_reject(L, pc);
  bad_pcbase = ((uint64_t)(uintptr_t)
	(proto_bc(pt)+pt->sizebc)) << 8;
  memcpy(&snapmap[1], &bad_pcbase, sizeof(bad_pcbase));
  expect_layout_reject(L, pc);
  memcpy(&snapmap[1], &saved_pcbase, sizeof(saved_pcbase));
  assert(root_entry_metadata_layout_valid(T));
}

static void test_metadata_mutation_rejections(lua_State *L, GCproto *pt,
					      RootEntryPatch *loop,
					      RootEntryMetadataFixture *fixture)
{
  jit_State *J = L2J(L);
  GCtrace *T = &J->cur;
  IRIns *ir = T->ir;
  SnapShot *snap = T->snap;
  BCIns current = (BCIns)la_load32_acq((const uint32_t *)loop->pc);

  trace_link_rel(T, 2);
  expect_metadata_reject(L, loop->pc);
  trace_link_rel(T, 1);

  trace_traceno_rel(T, 2);
  expect_metadata_reject(L, loop->pc);
  trace_traceno_rel(T, 1);

  T->root = 1;
  expect_metadata_reject(L, loop->pc);
  T->root = 0;

  T->linktype = LJ_TRLINK_ROOT;
  expect_metadata_reject(L, loop->pc);
  T->linktype = LJ_TRLINK_LOOP;

  T->nchild = 1;
  expect_metadata_reject(L, loop->pc);
  T->nchild = 0;

  trace_nextside_rel(T, 1);
  expect_metadata_reject(L, loop->pc);
  trace_nextside_rel(T, 0);

  test_spill_layout_mutations(L, loop->pc, fixture);
  test_snapshot_layout_mutations(L, loop->pc);

  T->mcloop = 0;
  expect_metadata_reject(L, loop->pc);
  T->mcloop = (MSize)sizeof(MCode);

  T->startins = BCINS_AJ(BC_LOOP, bc_a(loop->original), 0);
  expect_metadata_reject(L, loop->pc);
  T->startins = loop->original;

  T->unused1 &= (uint8_t)~TRACE_ARM64_INT_LOOP_ADMITTED;
  expect_metadata_reject(L, loop->pc);
  T->unused1 |= TRACE_ARM64_INT_LOOP_ADMITTED;

  T->retire_epoch = 1;
  expect_metadata_reject(L, loop->pc);
  T->retire_epoch = 0;

  T->unused1 |= TRACE_ENTRY_INVALIDATED;
  expect_metadata_reject(L, loop->pc);
  T->unused1 &= (uint8_t)~TRACE_ENTRY_INVALIDATED;

  T->ir = NULL;
  expect_metadata_reject(L, loop->pc);
  T->ir = ir;

  T->snap = NULL;
  expect_metadata_reject(L, loop->pc);
  T->snap = snap;

  T->topslot = (uint8_t)(pt->framesize-1u);
  expect_metadata_reject(L, loop->pc);
  T->topslot = pt->framesize;

  bc_publish(loop->pc, BCINS_AD(BC_JLOOP, bc_a(current), 2));
  expect_metadata_reject(L, loop->pc);
  bc_publish(loop->pc, current);
}

static void wait_for_pause(uint32_t stage, const RootEntryRace *race)
{
  uint32_t i;
  for (i = 0; i < 10000000u; i++) {
    if (lj_trace_test_root_entry_paused() == stage)
      return;
    assert(la_load32_acq(&race->worker_done) == 0);
    (void)lj_thr_retry_yield(NULL);
  }
  assert(!"root-entry helper did not reach requested pause");
}

static uint32_t root_entry_race_stage(RootEntryRaceMode mode)
{
  if (mode == ROOT_ENTRY_CLOSER_BEFORE_PUBLISH)
    return LJ_TRACE_ROOT_ENTRY_PAUSE_PREPUBLISH;
  if (mode == ROOT_ENTRY_CLOSER_AFTER_PUBLISH ||
      mode == ROOT_ENTRY_REQUEST_AFTER_PUBLISH)
    return LJ_TRACE_ROOT_ENTRY_PAUSE_POSTPUBLISH;
  return LJ_TRACE_ROOT_ENTRY_PAUSE_POSTMETADATA;
}

static void root_entry_publish_request(RootEntryRace *race)
{
  switch (race->mode) {
  case ROOT_ENTRY_POLL_AFTER_METADATA:
    lj_tg_poll_rel(race->tg, 1);
    break;
  case ROOT_ENTRY_REQMASK_AFTER_METADATA:
    lj_tg_reqmask_rel(race->tg, LJ_GC2_HS_REDISPATCH);
    break;
  case ROOT_ENTRY_REQUEST_AFTER_PUBLISH:
  case ROOT_ENTRY_PROFILE_AFTER_METADATA:
    lj_tg_profile_request_rel(race->tg, 1);
    break;
  default:
    assert(!"root-entry race has no request publication");
  }
}

static void *root_entry_closer(void *arg)
{
  RootEntryRace *race = (RootEntryRace *)arg;
  uint32_t stage = root_entry_race_stage(race->mode);
  wait_for_pause(stage, race);
  if (race->mode == ROOT_ENTRY_CLOSER_BEFORE_PUBLISH) {
    uint32_t i;
    for (i = 0; i < 10000000u; i++) {
      if (lj_gc2_test_idle_reclaim_enter(race->g)) {
        race->entered = 1;
        break;
      }
      (void)lj_thr_retry_yield(NULL);
    }
    assert(race->entered == 1);
    assert(gc2_jit_phase_gate_acq(race->g) == 0);
    lj_trace_test_root_entry_release();
    while (la_load32_acq(&race->entry_done) == 0)
      la_cpu_pause();
    lj_gc2_test_idle_reclaim_leave(race->g);
  } else if (race->mode == ROOT_ENTRY_CLOSER_AFTER_PUBLISH) {
    race->saw_active = (uint32_t)lj_tg_any_jit_active(race->g);
    race->entered = lj_gc2_test_idle_reclaim_enter(race->g);
    if (race->entered)
      lj_gc2_test_idle_reclaim_leave(race->g);
    lj_trace_test_root_entry_release();
  } else {
    root_entry_publish_request(race);
    lj_trace_test_root_entry_release();
  }
  la_store32_rel(&race->worker_done, 1);
  return NULL;
}

static void require_idle_reclaim_preflight(global_State *g)
{
  if (!lj_gc2_test_idle_reclaim_enter(g)) {
    fprintf(stderr, "root-entry idle preflight failed: phase=%u gate=%u "
      "active=%d smr=%u\n", gc2_phase_acq(g),
      gc2_jit_phase_gate_acq(g), lj_tg_any_jit_active(g),
      gc2_smr_reclaiming_acq(g));
    assert(0);
  }
  lj_gc2_test_idle_reclaim_leave(g);
}

static void run_pause_race(lua_State *L, BCIns *pc, RootEntryRaceMode mode)
{
  global_State *g = G(L);
  TGState *tg = L->tg_hint;
  RootEntryRace race = { g, tg, mode, 0, 0, 0, 0 };
  pthread_t closer;
  uint32_t publishes = lj_trace_test_root_entry_publishes();
  uint32_t cleanups = lj_trace_test_root_entry_cleanups();
  uint32_t stage = root_entry_race_stage(mode);
  LJTraceRootEntry entry;

  gc2_jit_sweep_displaced_rel(g, 0);
  lj_trace_test_root_entry_pause(stage);
  assert(pthread_create(&closer, NULL, root_entry_closer, &race) == 0);
  entry = test_trace_enter_root(L2J(L), pc, 1, L, L->base, BC_JLOOP);
  expect_reject(entry);
  assert(lj_tg_load_jit_base(tg) == NULL);
  la_store32_rel(&race.entry_done, 1);
  assert(pthread_join(closer, NULL) == 0);
  assert(la_load32_acq(&race.worker_done) == 1);
  assert(lj_trace_test_root_entry_publishes() == publishes + 1u);
  assert(lj_trace_test_root_entry_cleanups() == cleanups + 1u);
  if (mode == ROOT_ENTRY_CLOSER_BEFORE_PUBLISH) {
    assert(race.entered == 1);
    assert(gc2_jit_sweep_displaced_acq(g) == 0); /* Leave consumed it. */
  } else if (mode == ROOT_ENTRY_CLOSER_AFTER_PUBLISH) {
    assert(race.saw_active == 1);
    assert(race.entered == 0);
  } else if (mode == ROOT_ENTRY_REQUEST_AFTER_PUBLISH ||
	     mode == ROOT_ENTRY_PROFILE_AFTER_METADATA) {
    assert(lj_tg_profile_request_acq(tg) == 1);
    lj_tg_profile_request_rel(tg, 0);
  } else if (mode == ROOT_ENTRY_POLL_AFTER_METADATA) {
    assert(lj_tg_poll_acq(tg) == 1);
    lj_tg_poll_rel(tg, 0);
  } else {
    assert(mode == ROOT_ENTRY_REQMASK_AFTER_METADATA);
    assert(lj_tg_reqmask_acq(tg) == LJ_GC2_HS_REDISPATCH);
    lj_tg_reqmask_rel(tg, 0);
  }
  assert(gc2_jit_phase_gate_acq(g) != 0);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  jit_State *J;
  GCfunc *metadata_fn;
  GCproto *metadata_pt;
  RootEntryPatch metadata_loop;
  RootEntryFrameFixture frame_fixture;
  uint32_t publishes, cleanups;
  int32_t saved_vmstate;
  lua_State *tmpbuf_L;

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  tg = L->tg_hint;
  J = L2J(L);
  assert(g != NULL && tg != NULL && J != NULL);
  assert(tg == lj_thr_get_tg() && tg->gl == g);
  assert(lj_tg_load_cur_L(tg) == L && lj_tg_owns_state_acq(tg, L));
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_jit_phase_gate_acq(g) != 0);
  assert(tracevec_acq(J) == NULL); /* No root has recorded yet. */
  run_lua(L,
    "jit.off()\n"
    "function __arm64_root_metadata(n)\n"
    "  local i, x = 0, 0\n"
    "  while i < n do i, x = i + 1, x + i + 1 end\n"
    "  return x\n"
    "end\n");
  metadata_fn = global_lfunc(L, "__arm64_root_metadata");
  metadata_pt = funcproto(metadata_fn);
  metadata_loop = patch_first_root(L, "__arm64_root_metadata",
				   BC_LOOP, BC_JLOOP);
  assert(bc_j(metadata_loop.original) > 0);
  assert(bc_op(metadata_loop.pc[bc_j(metadata_loop.original)]) == BC_JMP);
  tmpbuf_L = sbufL(&tg->tmpbuf);
  lua_gc(L, LUA_GCSTOP, 0);
  require_idle_reclaim_preflight(g);
  lj_trace_test_root_entry_reset();
  install_root_entry_frame(L, metadata_fn, &frame_fixture);
  /* A direct C fixture starts in ~LJ_VMST_C. Emulate the exact VM state in
  ** which BC_JLOOP invokes this helper, then restore the harness state before
  ** exercising the real VM callers below. */
  saved_vmstate = lj_tg_vmstate_load_acq(tg);
  lj_tg_vmstate_store_rel(tg, (int32_t)~LJ_VMST_INTERP);

  /* Invalid calls reject before publication and never clear a foreign lease. */
  expect_reject(test_trace_enter_root(NULL, metadata_loop.pc, 1, L, L->base,
                                    BC_JLOOP));
  expect_reject(test_trace_enter_root(
    (jit_State *)((char *)J + sizeof(void *)), metadata_loop.pc, 1,
    L, L->base,
    BC_JLOOP));
  expect_reject(test_trace_enter_root(J, NULL, 1, L, L->base, BC_JLOOP));
  expect_reject(test_trace_enter_root(J, metadata_loop.pc, 0, L, L->base,
                                    BC_JLOOP));
  expect_reject(test_trace_enter_root(J, metadata_loop.pc, 1, NULL, L->base,
                                    BC_JLOOP));
  expect_reject(test_trace_enter_root(J, metadata_loop.pc, 1, L, NULL,
                                    BC_JLOOP));
  expect_reject(test_trace_enter_root(J, metadata_loop.pc, 1, L, L->base,
                                    BC_JFUNCV));
  {
    TValue *savedbase = L->base;
    TValue *stack = mref_acq(L->stack, TValue);
    assert(stack != NULL && L->top >= stack + LJ_FR2);
    L->base = stack + LJ_FR2;
    expect_reject(test_trace_enter_root(J, metadata_loop.pc, 1, L, L->base,
                                      BC_JLOOP));
    L->base = savedbase;
  }
  {
    TValue *savedbase = L->base;
    TValue *savedtop = L->top;
    TValue *stack = mref_acq(L->stack, TValue);
    TValue *maxstack = mref_acq(L->maxstack, TValue);
    TValue saved_func;
    assert(stack != NULL && maxstack != NULL && maxstack-stack >= 3);
    assert(metadata_pt->framesize > 1);
    copyTV(L, &saved_func, maxstack-3);
    setfuncV(L, maxstack-3, metadata_fn);
    L->base = maxstack-1;
    L->top = maxstack;
    assert(metadata_pt->framesize > (MSize)(maxstack-L->base));
    expect_reject(test_trace_enter_root(J, metadata_loop.pc, 1, L, L->base,
                                      BC_JLOOP));
    L->base = savedbase;
    L->top = savedtop;
    copyTV(L, maxstack-3, &saved_func);
  }
  lj_tg_store_jit_base(tg, L->base);
  expect_reject(test_trace_enter_root(J, metadata_loop.pc, 1, L, L->base,
                                    BC_JLOOP));
  assert(lj_tg_load_jit_base(tg) == L->base);
  lj_tg_store_jit_base(tg, NULL);
  assert(lj_tg_vmstate_load_acq(tg) == (int32_t)~LJ_VMST_INTERP);
  lj_tg_vmstate_store_rel(tg, (int32_t)~LJ_VMST_C);
  expect_reject(test_trace_enter_root(J, metadata_loop.pc, 1, L, L->base,
                                    BC_JLOOP));
  lj_tg_vmstate_store_rel(tg, (int32_t)~LJ_VMST_INTERP);
  lj_tg_vmstate_store_rel(tg, 1);
  expect_reject(test_trace_enter_root(J, metadata_loop.pc, 1, L, L->base,
                                    BC_JLOOP));
  lj_tg_vmstate_store_rel(tg, (int32_t)~LJ_VMST_INTERP);
  lj_tg_in_native_rel(tg, 1);
  expect_reject(test_trace_enter_root(J, metadata_loop.pc, 1, L, L->base,
                                    BC_JLOOP));
  lj_tg_in_native_rel(tg, 0);
  lj_tg_poll_rel(tg, 1);
  expect_reject(test_trace_enter_root(J, metadata_loop.pc, 1, L, L->base,
                                    BC_JLOOP));
  lj_tg_poll_rel(tg, 0);
  lj_tg_reqmask_rel(tg, LJ_GC2_HS_REDISPATCH);
  expect_reject(test_trace_enter_root(J, metadata_loop.pc, 1, L, L->base,
                                    BC_JLOOP));
  lj_tg_reqmask_rel(tg, 0);
  lj_tg_profile_request_rel(tg, 1);
  expect_reject(test_trace_enter_root(J, metadata_loop.pc, 1, L, L->base,
                                    BC_JLOOP));
  lj_tg_profile_request_rel(tg, 0);
  assert(lj_trace_test_root_entry_publishes() == 0);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(sbufL(&tg->tmpbuf) == tmpbuf_L);

  /* Every open root-source gate publishes intent before absent metadata
  ** reaches the one cleanup path. The incompatible JFUNCF/LOOP generation
  ** must reject after the same lifetime publication, never before it. */
  expect_reject(test_trace_enter_root(J, metadata_loop.pc, 1, L, L->base,
                                    BC_JLOOP));
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_root_entry_cleanups() == 1);
  publishes = lj_trace_test_root_entry_publishes();
  cleanups = lj_trace_test_root_entry_cleanups();
  expect_reject(test_trace_enter_root(J, metadata_loop.pc, 1, L, L->base,
                                    BC_JFUNCF));
  assert(lj_trace_test_root_entry_publishes() == publishes + 1u);
  assert(lj_trace_test_root_entry_cleanups() == cleanups + 1u);
  assert(sbufL(&tg->tmpbuf) == tmpbuf_L);

  /* A gate owner wins before publication: entry records displacement and does
  ** not claim a TG lifetime lease. */
  require_idle_reclaim_preflight(g);
  assert(lj_gc2_test_idle_reclaim_enter(g));
  publishes = lj_trace_test_root_entry_publishes();
  cleanups = lj_trace_test_root_entry_cleanups();
  gc2_jit_sweep_displaced_rel(g, 0);
  expect_reject(test_trace_enter_root(J, metadata_loop.pc, 1, L, L->base,
                                    BC_JLOOP));
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(gc2_jit_sweep_displaced_acq(g) == 1);
  assert(lj_trace_test_root_entry_publishes() == publishes);
  assert(lj_trace_test_root_entry_cleanups() == cleanups);
  assert(sbufL(&tg->tmpbuf) == tmpbuf_L);
  lj_gc2_test_idle_reclaim_leave(g);

  require_idle_reclaim_preflight(g);
  run_pause_race(L, metadata_loop.pc, ROOT_ENTRY_CLOSER_BEFORE_PUBLISH);
  require_idle_reclaim_preflight(g);
  run_pause_race(L, metadata_loop.pc, ROOT_ENTRY_CLOSER_AFTER_PUBLISH);
  run_pause_race(L, metadata_loop.pc, ROOT_ENTRY_REQUEST_AFTER_PUBLISH);
  {
    RootEntryMetadataFixture metadata;
    install_root_entry_metadata(J, metadata_pt, &metadata_loop, &metadata);
    expect_metadata_success(L, metadata_loop.pc, &metadata);
    test_metadata_mutation_rejections(L, metadata_pt, &metadata_loop,
				      &metadata);
    run_pause_race(L, metadata_loop.pc, ROOT_ENTRY_POLL_AFTER_METADATA);
    run_pause_race(L, metadata_loop.pc, ROOT_ENTRY_REQMASK_AFTER_METADATA);
    run_pause_race(L, metadata_loop.pc, ROOT_ENTRY_PROFILE_AFTER_METADATA);
    remove_root_entry_metadata(J, &metadata);
  }
  assert(tracevec_acq(J) == NULL && J->cur.traceno == 0);
  assert(sbufL(&tg->tmpbuf) == tmpbuf_L);
  remove_root_entry_frame(L, &frame_fixture);
  restore_root_patch(&metadata_loop);
  lj_tg_vmstate_store_rel(tg, saved_vmstate);

  /* Execute the checked-in BC_JLOOP and BC_JFUNCF VM callers themselves.
  ** The immutable startins sidecar supplies deterministic rejection recovery;
  ** no TraceVec slot or native target exists, so every open source gate
  ** publishes and cleans its rejected lifetime intent before recovery. */
  run_lua(L,
    "jit.off()\n"
    "function __arm64_root_loop(n)\n"
    "  local i, x = 0, 0\n"
    "  while i < n do i, x = i + 1, x + i + 1 end\n"
    "  return x\n"
    "end\n"
    "function __arm64_root_fixed(a, b, c)\n"
    "  return a * 3, b == nil, c == nil\n"
    "end\n"
    "local function __arm64_iter(t, k)\n"
    "  k = k + 1\n"
    "  local v = t[k]\n"
    "  if v ~= nil then return k, v end\n"
    "end\n"
    "function __arm64_root_iter(t)\n"
    "  local x = 0\n"
    "  for _, v in __arm64_iter, t, 0 do x = x + v end\n"
    "  return x\n"
    "end\n"
    "function __arm64_root_jforl(n)\n"
    "  local x = 0\n"
    "  for i = 1, n do x = x * 10 + i end\n"
    "  return x\n"
    "end\n"
    "function __arm64_root_jfori(n)\n"
    "  local x = 0\n"
    "  for i = 1, n do x = x * 10 + i end\n"
    "  return x\n"
    "end\n");
  {
    RootEntryPatch loop = patch_first_root(L, "__arm64_root_loop",
					  BC_LOOP, BC_JLOOP);
    RootEntryPatch fixed = patch_first_root(L, "__arm64_root_fixed",
					   BC_FUNCF, BC_JFUNCF);
    RootEntryPatch iter = patch_first_root(L, "__arm64_root_iter",
					  BC_ITERL, BC_JITERL);
    RootEntryNumericPatch jforl = patch_numeric_root(
      L, "__arm64_root_jforl", 0);
    RootEntryNumericPatch jfori = patch_numeric_root(
      L, "__arm64_root_jfori", 1);
    uint32_t loop_publishes, loop_cleanups;

    lj_trace_test_root_entry_reset();
    lj_trace_test_force_startins_retry(1);
    lj_trace_test_root_entry_retry_restore(loop.pc, loop.original);
    lua_pushinteger(L, 100);
    call_global(L, "__arm64_root_loop", 1, 1);
    assert(lua_tointeger(L, -1) == 5050);
    lua_pop(L, 1);
    loop_publishes = lj_trace_test_root_entry_publishes();
    loop_cleanups = lj_trace_test_root_entry_cleanups();
    assert(loop_publishes != 0 && loop_publishes == loop_cleanups);
    assert(lj_trace_test_root_entry_startins_calls() == 1);
    assert((BCIns)la_load32_acq((const uint32_t *)loop.pc) == loop.original);
    assert(lj_tg_load_jit_base(tg) == NULL && tracevec_acq(J) == NULL);

    lj_trace_test_root_entry_reset();
    lj_trace_test_force_startins_retry(1);
    lua_pushinteger(L, 14);
    call_global(L, "__arm64_root_fixed", 1, 3);
    assert(lua_tointeger(L, -3) == 42);
    assert(lua_toboolean(L, -2) != 0);
    assert(lua_toboolean(L, -1) != 0);
    lua_pop(L, 3);
    assert(lj_trace_test_root_entry_publishes() != 0);
    assert(lj_trace_test_root_entry_publishes() ==
	   lj_trace_test_root_entry_cleanups());
    assert(lj_tg_load_jit_base(tg) == NULL && tracevec_acq(J) == NULL);

    /* Preserve the pre-existing JITERL -> JLOOP tail path: it recovers its
    ** ITERL startins directly and never enters the strict-root helper. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_force_startins_retry(1);
    lua_createtable(L, 4, 0);
    lua_pushinteger(L, 3); lua_rawseti(L, -2, 1);
    lua_pushinteger(L, 6); lua_rawseti(L, -2, 2);
    lua_pushinteger(L, 9); lua_rawseti(L, -2, 3);
    lua_pushinteger(L, 12); lua_rawseti(L, -2, 4);
    call_global(L, "__arm64_root_iter", 1, 1);
    assert(lua_tointeger(L, -1) == 30);
    lua_pop(L, 1);
    assert(lj_trace_test_root_entry_publishes() == 0);
    assert(lj_trace_test_root_entry_cleanups() == 0);
    assert(lj_tg_load_jit_base(tg) == NULL && tracevec_acq(J) == NULL);

    /* Integer JFORL now attempts strict admission after increment/test/store.
    ** With no TraceVec installed each attempt must clean its lease, preserve
    ** the consumed JFORL generation and recover without executing FORL again. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_force_startins_retry(1);
    lua_pushinteger(L, 4);
    call_global(L, "__arm64_root_jforl", 1, 1);
    assert(lua_tointeger(L, -1) == 1234);
    lua_pop(L, 1);
    assert(lj_trace_test_root_entry_publishes() != 0);
    assert(lj_trace_test_root_entry_publishes() ==
	   lj_trace_test_root_entry_cleanups());
    assert(lj_trace_test_root_entry_startins_calls() != 0);
    lj_trace_test_root_entry_reset();
    lua_pushnumber(L, 4.5);
    call_global(L, "__arm64_root_jforl", 1, 1);
    assert(lua_tonumber(L, -1) == 1234.0);
    lua_pop(L, 1);
    assert(lj_trace_test_root_entry_publishes() == 0);
    assert(lj_trace_test_root_entry_cleanups() == 0);
    assert(lj_trace_test_root_entry_startins_calls() != 0);

    /* A synthetic paired JFORI reaches the same JFORL PC before executing the
    ** first body. Branch-only recovery must neither skip i=1 nor double-step
    ** later JFORL edges. Its later integer JFORL edges attempt admission, but
    ** the FP variant remains wholly branch-only. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_force_startins_retry(1);
    lua_pushinteger(L, 4);
    call_global(L, "__arm64_root_jfori", 1, 1);
    assert(lua_tointeger(L, -1) == 1234);
    lua_pop(L, 1);
    assert(lj_trace_test_root_entry_publishes() != 0);
    assert(lj_trace_test_root_entry_publishes() ==
	   lj_trace_test_root_entry_cleanups());
    assert(lj_trace_test_root_entry_startins_calls() != 0);
    lj_trace_test_root_entry_reset();
    lua_pushnumber(L, 4.5);
    call_global(L, "__arm64_root_jfori", 1, 1);
    assert(lua_tonumber(L, -1) == 1234.0);
    lua_pop(L, 1);
    assert(lj_trace_test_root_entry_publishes() == 0);
    assert(lj_trace_test_root_entry_cleanups() == 0);
    assert(lj_trace_test_root_entry_startins_calls() != 0);
    assert(lj_tg_load_jit_base(tg) == NULL && tracevec_acq(J) == NULL);

    restore_numeric_patch(&jfori);
    restore_numeric_patch(&jforl);
    restore_root_patch(&iter);
    restore_root_patch(&fixed);
    restore_root_patch(&loop);
  }

  lj_trace_test_root_entry_reset();
  lua_close(L);
  puts("arm64_jit_root_entry OK: strict LOOP/FORL/JFUNCF entry, source generations, mutations and request races verified");
  return 0;
}

#else

int main(void)
{
  puts("arm64_jit_root_entry SKIP: requires native experimental macOS ARM64");
  return 0;
}

#endif
