/*
** Synthetic, read-only contract for the first ARM64 side-recording ingress
** certificate. The broad side gate remains closed; the exact production
** first-side canary is open, but this fixture never updates a snapshot count
** or starts the recorder.
*/

#include <assert.h>
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
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_trace.h"

#if !LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED
#error "side ingress metadata checkpoint must land with recording closed"
#endif
#if LJ_ARM64_JIT_FIRST_SIDE_RECORDER_FAIL_CLOSED
#error "ordinary ARM64 helper build must expose the exact first-side canary"
#endif

enum {
  SIDE_META_PARENT = 1,
  SIDE_META_CHILD = 2,
  SIDE_META_EXIT = 2,
  SIDE_META_PC_POS = 13,
  SIDE_META_NSNAP = 8,
  SIDE_META_SECOND_EXIT = 6,
  SIDE_META_SECOND_PC_POS = 10,
  SIDE_META_SECOND_NSNAP = 9,
  SIDE_META_THIRD_EXIT = 7,
  SIDE_META_THIRD_PC_POS = 13,
  SIDE_META_THIRD_NSNAP = 11,
  SIDE_META_CAP_NSNAP = SIDE_META_THIRD_NSNAP,
  SIDE_META_NENT = 1,
  SIDE_META_FOOTER = 1 + LJ_FR2,
  SIDE_META_MAP_STRIDE = SIDE_META_NENT + SIDE_META_FOOTER,
  SIDE_META_NSNAPMAP = SIDE_META_NSNAP * SIDE_META_MAP_STRIDE,
  SIDE_META_CAP_NSNAPMAP = SIDE_META_CAP_NSNAP * SIDE_META_MAP_STRIDE,
  SIDE_META_BODY_WORDS = 4,
  SIDE_META_EXIT_WORDS = ARM64_EXIT_FALLBACK_WORDS +
    ARM64_EXIT_GATE_WORDS * SIDE_META_CAP_NSNAP,
  SIDE_META_MCODE_WORDS = SIDE_META_BODY_WORDS + SIDE_META_EXIT_WORDS,
  SIDE_META_R_VALUE = REF_FIRST,
  SIDE_META_R_END
};

typedef struct SideMetaTraceVec3 {
  MSize sizetrace;
  uint64_t retire_epoch;
  TraceVec *retired_next;
  GCRef slot[3];
} SideMetaTraceVec3;

typedef struct SideMetaFixture {
  GCtrace saved_cur;
  TraceNo saved_parent;
  ExitNo saved_exitno;
  TraceVec *saved_tracev;
  MSize saved_sizetrace;
  const BCIns *saved_pc;
  GCfunc *saved_fn;
  GCproto *saved_pt;
  int32_t saved_framedepth;
  int32_t saved_retdepth;
  BCReg saved_baseslot;
  LJTraceArm64SideParentCert saved_parent_cert;
  TValue saved_frame_function;
  GCtrace parent_trace;
  SideMetaTraceVec3 tracev;
  SideMetaTraceVec3 replacement_tracev;
  IRIns ir[SIDE_META_R_END];
  SnapShot snap[SIDE_META_CAP_NSNAP];
  SnapEntry snapmap[SIDE_META_CAP_NSNAPMAP];
  _Alignas(8) MCode mcode[SIDE_META_MCODE_WORDS];
  _Alignas(8) MCode *exittab[SIDE_META_CAP_NSNAP];
  GCfunc *fn;
  GCproto *pt;
  BCIns *looppc;
  BCIns *backpc;
  BCIns loopins;
  BCIns backins;
} SideMetaFixture;

static void run_lua(lua_State *L, const char *chunk)
{
  if (luaL_dostring(L, chunk) != 0) {
    fprintf(stderr, "side metadata Lua setup failed: %s\n",
            lua_tostring(L, -1));
    assert(0);
  }
}

static GCfunc *global_lfunc(lua_State *L, const char *name)
{
  GCfunc *fn;
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  fn = funcV(L->top-1);
  assert(isluafunc(fn));
  lua_pop(L, 1);
  return fn;
}

static void side_meta_pack_pc(SnapEntry *dst, const BCIns *pc,
                              uint8_t base_delta)
{
#if LJ_FR2
  uint64_t pcbase = ((uint64_t)(uintptr_t)pc << 8) | base_delta;
  memcpy(dst, &pcbase, sizeof(pcbase));
#else
  assert(base_delta == 0);
  *dst = (SnapEntry)(uintptr_t)pc;
#endif
}

static void side_meta_setir(IRIns *ir, IRRef ref, IROp op, IRType type,
                            IRRef op1, IRRef op2)
{
  memset(&ir[ref], 0, sizeof(ir[ref]));
  ir[ref].op1 = (IRRef1)op1;
  ir[ref].op2 = (IRRef1)op2;
  ir[ref].t.irt = (uint8_t)type;
  ir[ref].o = (IROp1)op;
  ir[ref].r = RID_X0;
  ir[ref].s = SPS_NONE;
}

static void side_meta_find_loop(SideMetaFixture *f)
{
  BCIns *bc = proto_bc(f->pt);
  BCPos pos;
  for (pos = 0; pos < f->pt->sizebc; pos++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[pos]);
    if (bc_op(ins) == BC_LOOP) {
      int64_t endpos = (int64_t)pos + (int64_t)bc_j(ins);
      assert(f->looppc == NULL && bc_j(ins) > 0);
      assert(endpos >= 0 && endpos < (int64_t)f->pt->sizebc);
      f->looppc = &bc[pos];
      f->loopins = ins;
      f->backpc = &bc[(BCPos)endpos];
      f->backins = (BCIns)la_load32_acq((const uint32_t *)f->backpc);
    }
  }
  assert(f->looppc != NULL && bc_op(f->backins) == BC_JMP &&
         bc_j(f->backins) < 0);
  assert(f->pt->sizebc > SIDE_META_PC_POS);
  assert(&bc[SIDE_META_PC_POS] >=
         f->backpc+1+bc_j(f->backins));
  assert(&bc[SIDE_META_PC_POS] < f->backpc+1);
}

static void side_meta_install(lua_State *L, SideMetaFixture *f)
{
  jit_State *J = L2J(L);
  GCtrace *T = &f->parent_trace;
  BCIns *bc;
  MCode *fallback, *gates;
  const BCPos footer_pc[SIDE_META_CAP_NSNAP] =
    { 3, 7, 13, 17, 3, 7, 10, 13, 3, 7, 3 };
  MSize i;

  memset(f, 0, sizeof(*f));
  f->fn = global_lfunc(L, "__arm64_side_meta_f");
  f->pt = funcproto(f->fn);
  bc = proto_bc(f->pt);
  side_meta_find_loop(f);
  assert(proto_bcpos(f->pt, &bc[SIDE_META_PC_POS]) == SIDE_META_PC_POS);

  f->saved_cur = J->cur;
  f->saved_parent = J->parent;
  f->saved_exitno = J->exitno;
  f->saved_tracev = tracevec_acq(J);
  f->saved_sizetrace = trace_sizetrace_acq(J);
  f->saved_pc = J->pc;
  f->saved_fn = J->fn;
  f->saved_pt = J->pt;
  f->saved_framedepth = J->framedepth;
  f->saved_retdepth = J->retdepth;
  f->saved_baseslot = J->baseslot;
  f->saved_parent_cert = J->arm64_side_parent;
  copyTV(L, &f->saved_frame_function, L->base-2);
  setfuncV(L, L->base-2, f->fn);
  assert(curr_func(L) == f->fn);

  T->gct = (uint32_t)~LJ_TTRACE;
  trace_traceno_rel(T, SIDE_META_PARENT);
  trace_link_rel(T, SIDE_META_PARENT);
  T->root = 0;
  T->linktype = LJ_TRLINK_LOOP;
  trace_nextside_rel(T, 0);
  trace_nchild_rel(T, 0);
  trace_startpt_rel(T, f->pt);
  setmref(T->startpc, f->looppc);
  T->startins = f->loopins;
  T->topslot = f->pt->framesize;
  T->unused1 = TRACE_ARM64_INT_LOOP_ADMITTED;
  T->ir = f->ir;
  T->nk = REF_TRUE;
  T->nins = SIDE_META_R_END;
  T->snap = f->snap;
  T->snapmap = f->snapmap;
  T->nsnap = SIDE_META_NSNAP;
  T->nsnapmap = SIDE_META_NSNAPMAP;
  T->mcode = f->mcode;
#if LJ_ABI_PAUTH
  la_storefunc_rel(&T->mcauth, lj_ptr_sign(
    ptrauth_nop_cast(ASMFunction, T->mcode), T));
#endif
  T->szmcode = SIDE_META_BODY_WORDS * (MSize)sizeof(MCode);
  T->mcloop = sizeof(MCode);
  fallback = f->mcode + SIDE_META_BODY_WORDS;
  gates = fallback + ARM64_EXIT_FALLBACK_WORDS;
  trace_exittab_rel(T, f->exittab);
  trace_exitstub_rel(T, gates);

  side_meta_setir(f->ir, REF_BASE, IR_BASE, IRT_PGC, 0, 0);
  side_meta_setir(f->ir, SIDE_META_R_VALUE, IR_SLOAD,
                  IRT_INT|IRT_GUARD, 2, IRSLOAD_TYPECHECK);
  for (i = 0; i < SIDE_META_CAP_NSNAP; i++) {
    MSize mapofs = i*SIDE_META_MAP_STRIDE;
    assert(footer_pc[i] < f->pt->sizebc);
    f->snap[i].mapofs = (uint32_t)mapofs;
    f->snap[i].ref = SIDE_META_R_VALUE;
    f->snap[i].nslots = f->pt->framesize;
    f->snap[i].topslot = f->pt->framesize;
    f->snap[i].nent = SIDE_META_NENT;
    f->snap[i].count = 0;
    f->snapmap[mapofs] = SNAP(4, 0, SIDE_META_R_VALUE);
    side_meta_pack_pc(&f->snapmap[mapofs+SIDE_META_NENT],
                      &bc[footer_pc[i]], 0);
    trace_exittarget_arm64_rel(G(L), T, (ExitNo)i, fallback);
  }
  /* The canonical live observation selects parent exit 2 and resumes at
  ** prototype bytecode offset 13. Keep those numbers explicit in this
  ** synthetic checkpoint even though no recorder is entered here. */
  side_meta_pack_pc(
    &f->snapmap[f->snap[SIDE_META_EXIT].mapofs+SIDE_META_NENT],
    &bc[SIDE_META_PC_POS], 0);

  proto_jit_startins_rel(f->pt, f->looppc, f->loopins);
  bc_publish(f->looppc,
             BCINS_AD(BC_JLOOP, bc_a(f->loopins), SIDE_META_PARENT));

  f->tracev.sizetrace = 3;
  setgcrefrel(f->tracev.slot[0], NULL);
  setgcrefrel(f->tracev.slot[SIDE_META_PARENT], obj2gco(T));
  setgcrefrel(f->tracev.slot[SIDE_META_CHILD],
              (const GCobj *)LJ_TRACE_PENDING);
  trace_sizetrace_rel(J, 3);
  tracevec_rel(J, (TraceVec *)&f->tracev);
}

static void side_meta_remove(lua_State *L, SideMetaFixture *f)
{
  jit_State *J = L2J(L);
  assert(jit_token_acq(G(L)) == 0 && jit_owner_l_acq(J) == NULL);
  bc_publish(f->looppc, f->loopins);
  tracevec_rel(J, f->saved_tracev);
  trace_sizetrace_rel(J, f->saved_sizetrace);
  J->cur = f->saved_cur;
  J->parent = f->saved_parent;
  J->exitno = f->saved_exitno;
  J->pc = f->saved_pc;
  J->fn = f->saved_fn;
  J->pt = f->saved_pt;
  J->framedepth = f->saved_framedepth;
  J->retdepth = f->saved_retdepth;
  J->baseslot = f->saved_baseslot;
  J->arm64_side_parent = f->saved_parent_cert;
  copyTV(L, L->base-2, &f->saved_frame_function);
}

static int side_meta_check_at(lua_State *L, SideMetaFixture *f,
                              const BCIns *continuation, const BCIns *pc,
                              uint32_t context)
{
  uint8_t before = f->snap[SIDE_META_EXIT].count;
  int ok = lj_trace_test_arm64_first_side_loop_valid(
    L2J(L), context == LJ_TRACE_ARM64_SIDE_CONTEXT_METADATA ? NULL : L,
    SIDE_META_PARENT, SIDE_META_EXIT, continuation, pc, context);
  assert(f->snap[SIDE_META_EXIT].count == before);
  return ok;
}

static int side_meta_check(lua_State *L, SideMetaFixture *f,
                           const BCIns *pc, uint32_t context)
{
  return side_meta_check_at(L, f, pc, pc, context);
}

static void test_second_descriptor(lua_State *L, SideMetaFixture *f)
{
  jit_State *J = L2J(L);
  GCtrace *T = &f->parent_trace;
  const BCIns *continuation =
    &proto_bc(f->pt)[SIDE_META_SECOND_PC_POS];
  SnapShot *selected = &f->snap[SIDE_META_SECOND_EXIT];
  MSize footer = selected->mapofs+selected->nent;
  SnapEntry saved_footer[SIDE_META_FOOTER];
  MSize saved_nsnap = T->nsnap;
  MSize saved_nsnapmap = T->nsnapmap;
  TraceNo saved_parent = J->parent;
  ExitNo saved_exitno = J->exitno;
  uint8_t before = selected->count;

  memcpy(saved_footer, &f->snapmap[footer], sizeof(saved_footer));
  side_meta_pack_pc(&f->snapmap[footer], continuation, 0);
  /* Exit 6 is not a free-standing alternative: its parent snapshot count and
  ** continuation offset belong to the same exact descriptor. */
  assert(!lj_trace_test_arm64_first_side_loop_valid(
    J, NULL, SIDE_META_PARENT, SIDE_META_SECOND_EXIT, continuation, NULL,
    LJ_TRACE_ARM64_SIDE_CONTEXT_METADATA));
  T->nsnap = SIDE_META_SECOND_NSNAP;
  T->nsnapmap = SIDE_META_CAP_NSNAPMAP;
  assert(!lj_trace_test_arm64_first_side_loop_valid(
    J, NULL, SIDE_META_PARENT, SIDE_META_EXIT,
    &proto_bc(f->pt)[SIDE_META_PC_POS], NULL,
    LJ_TRACE_ARM64_SIDE_CONTEXT_METADATA));
  assert(!lj_trace_test_arm64_first_side_loop_valid(
    J, NULL, SIDE_META_PARENT, SIDE_META_SECOND_EXIT,
    &proto_bc(f->pt)[SIDE_META_PC_POS], NULL,
    LJ_TRACE_ARM64_SIDE_CONTEXT_METADATA));
  assert(lj_trace_test_arm64_first_side_loop_valid(
    J, NULL, SIDE_META_PARENT, SIDE_META_SECOND_EXIT, continuation, NULL,
    LJ_TRACE_ARM64_SIDE_CONTEXT_METADATA));
  assert(lj_trace_test_arm64_first_side_loop_valid(
    J, L, SIDE_META_PARENT, SIDE_META_SECOND_EXIT,
    continuation, continuation,
    LJ_TRACE_ARM64_SIDE_CONTEXT_IDLE));

  assert(lj_jit_token_try_l(L, J));
  assert(lj_trace_test_arm64_first_side_loop_valid(
    J, L, SIDE_META_PARENT, SIDE_META_SECOND_EXIT,
    continuation, continuation,
    LJ_TRACE_ARM64_SIDE_CONTEXT_CLAIM));
  jit_owner_l_rel(J, L);
  J->parent = SIDE_META_PARENT;
  J->exitno = SIDE_META_SECOND_EXIT;
  lj_trace_state_store(J, LJ_TRACE_START);
  assert(lj_trace_test_arm64_first_side_loop_valid(
    J, L, SIDE_META_PARENT, SIDE_META_SECOND_EXIT,
    continuation, continuation,
    LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  lj_trace_state_store(J, LJ_TRACE_IDLE);
  jit_owner_l_rel(J, NULL);
  J->parent = saved_parent;
  J->exitno = saved_exitno;
  lj_jit_token_release_l(L, J);

  assert(selected->count == before);
  T->nsnap = saved_nsnap;
  T->nsnapmap = saved_nsnapmap;
  memcpy(&f->snapmap[footer], saved_footer, sizeof(saved_footer));
  assert(jit_token_acq(G(L)) == 0 && jit_owner_l_acq(J) == NULL &&
         lj_trace_state_load(J) == LJ_TRACE_IDLE);
}

static void test_third_descriptor(lua_State *L, SideMetaFixture *f)
{
  jit_State *J = L2J(L);
  GCtrace *T = &f->parent_trace;
  const BCIns *continuation =
    &proto_bc(f->pt)[SIDE_META_THIRD_PC_POS];
  SnapShot *selected = &f->snap[SIDE_META_THIRD_EXIT];
  MSize footer = selected->mapofs+selected->nent;
  SnapEntry saved_footer[SIDE_META_FOOTER];
  MSize saved_nsnap = T->nsnap;
  MSize saved_nsnapmap = T->nsnapmap;
  TraceNo saved_parent = J->parent;
  ExitNo saved_exitno = J->exitno;
  uint8_t before = selected->count;

  memcpy(saved_footer, &f->snapmap[footer], sizeof(saved_footer));
  side_meta_pack_pc(&f->snapmap[footer], continuation, 0);
  /* Exit 7 couples the first descriptor's continuation geometry with a
  ** separately observed eleven-snapshot parent. Neither half is admitted on
  ** its own, and the other known exits remain tied to their own parent shape. */
  assert(!lj_trace_test_arm64_first_side_loop_valid(
    J, NULL, SIDE_META_PARENT, SIDE_META_THIRD_EXIT, continuation, NULL,
    LJ_TRACE_ARM64_SIDE_CONTEXT_METADATA));
  T->nsnap = SIDE_META_THIRD_NSNAP;
  T->nsnapmap = SIDE_META_CAP_NSNAPMAP;
  assert(!lj_trace_test_arm64_first_side_loop_valid(
    J, NULL, SIDE_META_PARENT, SIDE_META_EXIT,
    &proto_bc(f->pt)[SIDE_META_PC_POS], NULL,
    LJ_TRACE_ARM64_SIDE_CONTEXT_METADATA));
  assert(!lj_trace_test_arm64_first_side_loop_valid(
    J, NULL, SIDE_META_PARENT, SIDE_META_SECOND_EXIT,
    &proto_bc(f->pt)[SIDE_META_SECOND_PC_POS], NULL,
    LJ_TRACE_ARM64_SIDE_CONTEXT_METADATA));
  assert(!lj_trace_test_arm64_first_side_loop_valid(
    J, NULL, SIDE_META_PARENT, SIDE_META_THIRD_EXIT,
    &proto_bc(f->pt)[SIDE_META_SECOND_PC_POS], NULL,
    LJ_TRACE_ARM64_SIDE_CONTEXT_METADATA));
  assert(lj_trace_test_arm64_first_side_loop_valid(
    J, NULL, SIDE_META_PARENT, SIDE_META_THIRD_EXIT, continuation, NULL,
    LJ_TRACE_ARM64_SIDE_CONTEXT_METADATA));
  assert(lj_trace_test_arm64_first_side_loop_valid(
    J, L, SIDE_META_PARENT, SIDE_META_THIRD_EXIT,
    continuation, continuation, LJ_TRACE_ARM64_SIDE_CONTEXT_IDLE));

  assert(lj_jit_token_try_l(L, J));
  assert(lj_trace_test_arm64_first_side_loop_valid(
    J, L, SIDE_META_PARENT, SIDE_META_THIRD_EXIT,
    continuation, continuation, LJ_TRACE_ARM64_SIDE_CONTEXT_CLAIM));
  jit_owner_l_rel(J, L);
  J->parent = SIDE_META_PARENT;
  J->exitno = SIDE_META_THIRD_EXIT;
  lj_trace_state_store(J, LJ_TRACE_START);
  assert(lj_trace_test_arm64_first_side_loop_valid(
    J, L, SIDE_META_PARENT, SIDE_META_THIRD_EXIT,
    continuation, continuation, LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  lj_trace_state_store(J, LJ_TRACE_IDLE);
  jit_owner_l_rel(J, NULL);
  J->parent = saved_parent;
  J->exitno = saved_exitno;
  lj_jit_token_release_l(L, J);

  assert(selected->count == before);
  T->nsnap = saved_nsnap;
  T->nsnapmap = saved_nsnapmap;
  memcpy(&f->snapmap[footer], saved_footer, sizeof(saved_footer));
  assert(T->nsnap == saved_nsnap && T->nsnapmap == saved_nsnapmap);
  assert(memcmp(&f->snapmap[footer], saved_footer,
                sizeof(saved_footer)) == 0);
  assert(J->parent == saved_parent && J->exitno == saved_exitno);
  assert(jit_token_acq(G(L)) == 0 && jit_owner_l_acq(J) == NULL &&
         lj_trace_state_load(J) == LJ_TRACE_IDLE);
}

static void expect_metadata_reject(lua_State *L, SideMetaFixture *f)
{
  assert(!side_meta_check(L, f, &proto_bc(f->pt)[SIDE_META_PC_POS],
                          LJ_TRACE_ARM64_SIDE_CONTEXT_METADATA));
}

static void test_metadata_mutations(lua_State *L, SideMetaFixture *f)
{
  jit_State *J = L2J(L);
  GCtrace *T = &f->parent_trace;
  BCIns *bc = proto_bc(f->pt);
  SnapShot *selected = &f->snap[SIDE_META_EXIT];
  MSize footer = selected->mapofs+selected->nent;
  uint64_t saved_pcbase, bad_pcbase;
  SnapEntry saved_entry = f->snapmap[selected->mapofs];
  MCode *saved_mcode = T->mcode;
  MCode **saved_exittab = T->exittab;
  MCode *saved_exitstub = T->exitstub;
  void *saved_exittarget = la_loadptr_acq(
    (void *const *)&f->exittab[SIDE_META_EXIT]);
  MSize saved_szmcode = T->szmcode;

  assert(side_meta_check(L, f, &bc[SIDE_META_PC_POS],
                         LJ_TRACE_ARM64_SIDE_CONTEXT_METADATA));
  assert(!lj_trace_test_arm64_first_side_loop_valid(
    J, NULL, 0, SIDE_META_EXIT, &bc[SIDE_META_PC_POS], NULL,
    LJ_TRACE_ARM64_SIDE_CONTEXT_METADATA));
  assert(!lj_trace_test_arm64_first_side_loop_valid(
    J, NULL, SIDE_META_PARENT, SIDE_META_NSNAP, &bc[SIDE_META_PC_POS], NULL,
    LJ_TRACE_ARM64_SIDE_CONTEXT_METADATA));

  setgcrefrel(f->tracev.slot[SIDE_META_PARENT], NULL);
  expect_metadata_reject(L, f);
  setgcrefrel(f->tracev.slot[SIDE_META_PARENT], obj2gco(T));

  trace_traceno_rel(T, 2);
  expect_metadata_reject(L, f);
  trace_traceno_rel(T, SIDE_META_PARENT);
  T->retire_epoch = 1;
  expect_metadata_reject(L, f);
  T->retire_epoch = 0;
  T->unused1 |= TRACE_ENTRY_INVALIDATED;
  expect_metadata_reject(L, f);
  T->unused1 &= (uint8_t)~TRACE_ENTRY_INVALIDATED;

  T->unused1 &= (uint8_t)~TRACE_ARM64_INT_LOOP_ADMITTED;
  expect_metadata_reject(L, f);
  T->unused1 |= TRACE_ARM64_INT_LOOP_ADMITTED;
  T->unused1 |= TRACE_ARM64_INT_FORL_ADMITTED;
  expect_metadata_reject(L, f);
  T->unused1 &= (uint8_t)~TRACE_ARM64_INT_FORL_ADMITTED;
  T->unused1 |= TRACE_ARM64_INT_SIDE_ADMITTED;
  expect_metadata_reject(L, f);
  T->unused1 &= (uint8_t)~TRACE_ARM64_INT_SIDE_ADMITTED;

  T->root = SIDE_META_PARENT;
  expect_metadata_reject(L, f);
  T->root = 0;
  trace_link_rel(T, 2);
  expect_metadata_reject(L, f);
  trace_link_rel(T, SIDE_META_PARENT);
  T->linktype = LJ_TRLINK_ROOT;
  expect_metadata_reject(L, f);
  T->linktype = LJ_TRLINK_LOOP;
  trace_nchild_rel(T, 1);
  expect_metadata_reject(L, f);
  trace_nchild_rel(T, 0);
  trace_nextside_rel(T, 2);
  expect_metadata_reject(L, f);
  trace_nextside_rel(T, 0);

  trace_startpt_clear(T);
  expect_metadata_reject(L, f);
  trace_startpt_rel(T, f->pt);
  setmref(T->startpc, f->looppc+1);
  expect_metadata_reject(L, f);
  setmref(T->startpc, f->looppc);
  T->startins = BCINS_AJ(BC_LOOP, bc_a(f->loopins), 0);
  expect_metadata_reject(L, f);
  T->startins = f->loopins;
  T->spadjust = 16;
  expect_metadata_reject(L, f);
  T->spadjust = 0;
  T->topslot--;
  expect_metadata_reject(L, f);
  T->topslot = f->pt->framesize;

  bc_publish(f->looppc, f->loopins);
  expect_metadata_reject(L, f);
  bc_publish(f->looppc,
             BCINS_AD(BC_JLOOP, bc_a(f->loopins), SIDE_META_PARENT));
  bc_publish(f->looppc,
             BCINS_AD(BC_JLOOP, bc_a(f->loopins), 2));
  expect_metadata_reject(L, f);
  bc_publish(f->looppc,
             BCINS_AD(BC_JLOOP, bc_a(f->loopins), SIDE_META_PARENT));
  bc_publish(f->backpc, BCINS_AJ(BC_JMP, bc_a(f->backins), 0));
  expect_metadata_reject(L, f);
  bc_publish(f->backpc, f->backins);

  T->mcode = NULL;
  expect_metadata_reject(L, f);
  T->mcode = saved_mcode;
  trace_exittab_rel(T, NULL);
  expect_metadata_reject(L, f);
  trace_exittab_rel(T, saved_exittab);
  trace_exitstub_rel(T, NULL);
  expect_metadata_reject(L, f);
  trace_exitstub_rel(T, saved_exitstub);
  trace_exitstub_rel(T, saved_exitstub+1);
  expect_metadata_reject(L, f);
  trace_exitstub_rel(T, saved_exitstub);
  T->szmcode += 2u*sizeof(MCode);
  expect_metadata_reject(L, f);
  T->szmcode = saved_szmcode;
  T->unused1 |= TRACE_EXITTAB_MCODE;
  expect_metadata_reject(L, f);
  T->unused1 &= (uint8_t)~TRACE_EXITTAB_MCODE;
  trace_exittarget_arm64_rel(G(L), T, SIDE_META_EXIT, T->mcode);
  expect_metadata_reject(L, f);
  la_storeptr_rel((void **)&f->exittab[SIDE_META_EXIT], saved_exittarget);
#if LJ_ABI_PAUTH
  la_storeptr_rel((void **)&f->exittab[SIDE_META_EXIT],
    trace_exittarget_arm64_encode((global_State *)(void *)T,
      exitstub_trace_fallback_addr_(saved_exitstub)));
  expect_metadata_reject(L, f);
  la_storeptr_rel((void **)&f->exittab[SIDE_META_EXIT], saved_exittarget);
#endif
  T->snap = NULL;
  expect_metadata_reject(L, f);
  T->snap = f->snap;
  T->snapmap = NULL;
  expect_metadata_reject(L, f);
  T->snapmap = f->snapmap;

  selected->count = SNAPCOUNT_DONE;
  expect_metadata_reject(L, f);
  selected->count = 0;
  selected->mapofs = SIDE_META_NSNAPMAP;
  expect_metadata_reject(L, f);
  selected->mapofs = SIDE_META_EXIT*SIDE_META_MAP_STRIDE;
  f->snap[SIDE_META_EXIT+1].mapofs++;
  expect_metadata_reject(L, f);
  f->snap[SIDE_META_EXIT+1].mapofs--;
  selected->nslots = 1;
  expect_metadata_reject(L, f);
  selected->nslots = f->pt->framesize;
  selected->topslot = (uint8_t)(f->pt->framesize+1u);
  expect_metadata_reject(L, f);
  selected->topslot = f->pt->framesize;
  f->snapmap[selected->mapofs] = SNAP(selected->nslots, 0,
                                      SIDE_META_R_VALUE);
  expect_metadata_reject(L, f);
  f->snapmap[selected->mapofs] = saved_entry | SNAP_NORESTORE;
  expect_metadata_reject(L, f);
  f->snapmap[selected->mapofs] = saved_entry | UINT32_C(0x00800000);
  expect_metadata_reject(L, f);
  f->snapmap[selected->mapofs] = SNAP(2, 0, SIDE_META_R_VALUE);
  expect_metadata_reject(L, f);
  f->snapmap[selected->mapofs] = SNAP(4, 0, REF_DROP);
  expect_metadata_reject(L, f);
  f->snapmap[selected->mapofs] = saved_entry;

#if LJ_FR2
  memcpy(&saved_pcbase, &f->snapmap[footer], sizeof(saved_pcbase));
  bad_pcbase = saved_pcbase | UINT64_C(1);
  memcpy(&f->snapmap[footer], &bad_pcbase, sizeof(bad_pcbase));
  expect_metadata_reject(L, f);
  bad_pcbase = (uint64_t)(uintptr_t)&bc[SIDE_META_PC_POS-1] << 8;
  memcpy(&f->snapmap[footer], &bad_pcbase, sizeof(bad_pcbase));
  expect_metadata_reject(L, f);
  bad_pcbase = (uint64_t)(uintptr_t)&bc[1] << 8;
  memcpy(&f->snapmap[footer], &bad_pcbase, sizeof(bad_pcbase));
  assert(!side_meta_check(L, f, &bc[1],
                          LJ_TRACE_ARM64_SIDE_CONTEXT_METADATA));
  memcpy(&f->snapmap[footer], &saved_pcbase, sizeof(saved_pcbase));
#else
  (void)saved_pcbase;
  (void)bad_pcbase;
#endif
  assert(side_meta_check(L, f, &bc[SIDE_META_PC_POS],
                         LJ_TRACE_ARM64_SIDE_CONTEXT_METADATA));
}

static void test_context_mutations(lua_State *L, SideMetaFixture *f)
{
  jit_State *J = L2J(L);
  TGState *tg = L->tg_hint;
  BCIns *pc = &proto_bc(f->pt)[SIDE_META_PC_POS];
  TValue exact_frame;

  assert(tg != NULL && jit_token_acq(G(L)) == 0 &&
         lj_trace_state_load(J) == LJ_TRACE_IDLE && jit_owner_l_acq(J) == NULL);
  assert(side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_IDLE));
  copyTV(L, &exact_frame, L->base-2);
  copyTV(L, L->base-2, &f->saved_frame_function);
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_IDLE));
  copyTV(L, L->base-2, &exact_frame);

  lj_tg_poll_rel(tg, 1);
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_IDLE));
  lj_tg_poll_rel(tg, 0);
  lj_tg_reqmask_rel(tg, 1);
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_IDLE));
  lj_tg_reqmask_rel(tg, 0);
  lj_tg_profile_request_rel(tg, 1);
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_IDLE));
  lj_tg_profile_request_rel(tg, 0);
  (void)lj_tg_hookmask_update(tg, 0, HOOK_VMEVENT);
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_IDLE));
  (void)lj_tg_hookmask_update(tg, HOOK_VMEVENT, 0);
  lj_tg_store_cur_L(tg, NULL);
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_IDLE));
  lj_tg_store_cur_L(tg, L);

  /* The claim checkpoint is still an IDLE recorder with no published owner or
  ** selectors, but the exact TG owns the low token. Exercise that deliberately
  ** narrow state independently of the later START/RECORD owner checkpoint. */
  assert(lj_jit_token_try_l(L, J));
  assert(side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_CLAIM));
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_IDLE));
  assert(!side_meta_check_at(L, f, pc, pc+1,
                             LJ_TRACE_ARM64_SIDE_CONTEXT_CLAIM));
  assert(!side_meta_check_at(L, f, pc+1, pc+1,
                             LJ_TRACE_ARM64_SIDE_CONTEXT_CLAIM));
  lj_tg_reqmask_rel(tg, 1);
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_CLAIM));
  lj_tg_reqmask_rel(tg, 0);
  jit_owner_l_rel(J, L);
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_CLAIM));
  jit_owner_l_rel(J, NULL);
  lj_trace_state_store(J, LJ_TRACE_START);
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_CLAIM));
  lj_trace_state_store(J, LJ_TRACE_IDLE);
  lj_jit_token_release_l(L, J);
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_CLAIM));

  assert(lj_jit_token_try_l(L, J));
  lj_trace_state_store(J, LJ_TRACE_START);
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  jit_owner_l_rel(J, L);
  J->parent = SIDE_META_PARENT;
  J->exitno = SIDE_META_EXIT;
  J->pc = pc-1;
  J->fn = NULL;
  J->pt = NULL;
  J->framedepth = 7;
  J->retdepth = 9;
  J->baseslot = 31;
  assert(side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  assert(!side_meta_check_at(L, f, pc, pc+1,
                             LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  assert(!side_meta_check_at(L, f, pc+1, pc+1,
                             LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  assert(!side_meta_check_at(L, f,
                             &proto_bc(f->pt)[f->pt->sizebc], pc,
                             LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_IDLE));
  J->parent = SIDE_META_PARENT+1;
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  J->parent = SIDE_META_PARENT;
  J->exitno = SIDE_META_EXIT-1;
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  J->exitno = SIDE_META_EXIT;
  jit_owner_l_rel(J, NULL);
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  jit_owner_l_rel(J, L);

  memset(&J->cur, 0, sizeof(J->cur));
  trace_traceno_rel(&J->cur, 2);
  J->cur.root = SIDE_META_PARENT;
  trace_startpt_rel(&J->cur, f->pt);
  setmref(J->cur.startpc, pc);
  J->cur.startins = BCINS_AD(BC_JMP, 0, 0);
  J->pc = pc-1;
  J->fn = f->fn;
  J->pt = f->pt;
  J->framedepth = 0;
  J->retdepth = 0;
  J->baseslot = (BCReg)(1+LJ_FR2);
  lj_trace_state_store(J, LJ_TRACE_RECORD);
  assert(side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  assert(side_meta_check_at(L, f, pc, pc+1,
                            LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  assert(!side_meta_check_at(L, f, pc,
                             &proto_bc(f->pt)[f->pt->sizebc],
                             LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  setmref(J->cur.startpc, pc+1);
  assert(!side_meta_check_at(L, f, pc, pc+1,
                             LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  setmref(J->cur.startpc, pc);
  trace_traceno_rel(&J->cur, SIDE_META_PARENT);
  assert(!side_meta_check_at(L, f, pc, pc+1,
                             LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  trace_traceno_rel(&J->cur, 2);
  J->cur.startins = BCINS_AD(BC_JMP, 1, 0);
  assert(!side_meta_check_at(L, f, pc, pc+1,
                             LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  J->cur.startins = BCINS_AD(BC_JMP, 0, 0);
  J->baseslot++;
  assert(!side_meta_check_at(L, f, pc, pc+1,
                             LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  J->baseslot--;
  J->framedepth = 1;
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  J->framedepth = 0;
  J->retdepth = 1;
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  J->retdepth = 0;
  J->pt = NULL;
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  J->pt = f->pt;
  J->cur.root = 0;
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  J->cur.root = SIDE_META_PARENT;
  lj_trace_state_store(J, LJ_TRACE_RECORD_1ST);
  assert(!side_meta_check_at(L, f, pc, pc+1,
                             LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));
  lj_trace_state_store(J, LJ_TRACE_ASM);
  assert(!side_meta_check(L, f, pc, LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER));

  lj_trace_state_store(J, LJ_TRACE_IDLE);
  lj_jit_token_release_l(L, J);
  assert(jit_token_acq(G(L)) == 0 && jit_owner_l_acq(J) == NULL);
}

static int side_parent_cert_empty(const LJTraceArm64SideParentCert *cert)
{
  return cert->tracev == NULL && cert->body == NULL && cert->mcode == NULL &&
    cert->continuation == NULL && cert->continuationins == 0 &&
    cert->parent == 0 && cert->exitno == 0 && cert->child == 0;
}

static int side_parent_cert_equal(const LJTraceArm64SideParentCert *a,
                                  const LJTraceArm64SideParentCert *b)
{
  return a->tracev == b->tracev && a->body == b->body &&
    a->mcode == b->mcode &&
    a->continuation == b->continuation &&
    a->continuationins == b->continuationins &&
    a->parent == b->parent && a->exitno == b->exitno &&
    a->child == b->child;
}

static void side_parent_prepare_asm(lua_State *L, SideMetaFixture *f)
{
  jit_State *J = L2J(L);
  const BCIns *continuation =
    &proto_bc(f->pt)[SIDE_META_PC_POS];

  assert(lj_jit_token_try_l(L, J));
  jit_owner_l_rel(J, L);
  J->parent = SIDE_META_PARENT;
  J->exitno = SIDE_META_EXIT;
  J->pc = continuation+1;
  J->fn = f->fn;
  J->pt = f->pt;
  J->framedepth = 0;
  J->retdepth = 0;
  J->baseslot = (BCReg)(1+LJ_FR2);
  memset(&J->cur, 0, sizeof(J->cur));
  trace_traceno_rel(&J->cur, SIDE_META_PARENT+1);
  J->cur.root = SIDE_META_PARENT;
  trace_link_rel(&J->cur, SIDE_META_PARENT);
  J->cur.linktype = LJ_TRLINK_ROOT;
  trace_startpt_rel(&J->cur, f->pt);
  setmref(J->cur.startpc, continuation);
  J->cur.startins = BCINS_AD(BC_JMP, 0, 0);
  lj_trace_state_store(J, LJ_TRACE_ASM);
}

static void test_parent_lifetime_certificate(lua_State *L, SideMetaFixture *f)
{
  jit_State *J = L2J(L);
  global_State *g = G(L);
  GCtrace *T = &f->parent_trace;
  const BCIns *continuation = &proto_bc(f->pt)[SIDE_META_PC_POS];
  LJTraceArm64SideParentCert cert, before, sentinel;
  GCtrace replacement;
  SnapEntry saved_footer[SIDE_META_FOOTER];
  MSize footer = f->snap[SIDE_META_EXIT].mapofs+
    f->snap[SIDE_META_EXIT].nent;
  BCIns saved_continuationins =
    (BCIns)la_load32_acq((const uint32_t *)continuation);
  uint32_t expect;

  LJ_STATIC_ASSERT(LJ_TRACE_ARM64_SIDE_PARENT_SMR_RETRY == -1);
  LJ_STATIC_ASSERT(LJ_TRACE_ARM64_SIDE_PARENT_RETRY == 0);
  LJ_STATIC_ASSERT(LJ_TRACE_ARM64_SIDE_PARENT_OK == 1);

  /* An unauthorized call cannot clear token-private state. */
  sentinel.tracev = (TraceVec *)&f->tracev;
  sentinel.body = T;
  sentinel.mcode = T->mcode;
  sentinel.continuation = continuation;
  sentinel.continuationins = saved_continuationins;
  sentinel.parent = SIDE_META_PARENT;
  sentinel.exitno = SIDE_META_EXIT;
  sentinel.child = SIDE_META_CHILD;
  J->arm64_side_parent = sentinel;
  assert(lj_trace_arm64_side_parent_capture(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_RETRY);
  assert(side_parent_cert_equal(&J->arm64_side_parent, &sentinel));
  lj_trace_arm64_side_parent_clear(J);
  assert(side_parent_cert_empty(&J->arm64_side_parent));

  side_parent_prepare_asm(L, f);
  assert(lj_trace_arm64_side_parent_capture(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_OK);
  cert = J->arm64_side_parent;
  assert(cert.tracev == (TraceVec *)&f->tracev &&
         cert.body == T && cert.mcode == T->mcode &&
         cert.continuation == continuation &&
         cert.continuationins == saved_continuationins &&
         cert.parent == SIDE_META_PARENT && cert.exitno == SIDE_META_EXIT &&
         cert.child == SIDE_META_CHILD);
  assert(lj_trace_arm64_side_parent_revalidate(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_OK);

  /* Every stored identity component is independently authoritative. */
  J->arm64_side_parent.tracev = (TraceVec *)&f->replacement_tracev;
  assert(lj_trace_arm64_side_parent_revalidate(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_RETRY);
  J->arm64_side_parent = cert;
  J->arm64_side_parent.body = &replacement;
  assert(lj_trace_arm64_side_parent_revalidate(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_RETRY);
  J->arm64_side_parent = cert;
  J->arm64_side_parent.mcode = cert.mcode+1;
  assert(lj_trace_arm64_side_parent_revalidate(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_RETRY);
  J->arm64_side_parent = cert;
  J->arm64_side_parent.continuation = continuation+1;
  assert(lj_trace_arm64_side_parent_revalidate(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_RETRY);
  J->arm64_side_parent = cert;
  J->arm64_side_parent.continuationins ^= UINT32_C(0x00000100);
  assert(lj_trace_arm64_side_parent_revalidate(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_RETRY);
  J->arm64_side_parent = cert;
  J->arm64_side_parent.parent++;
  assert(lj_trace_arm64_side_parent_revalidate(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_RETRY);
  J->arm64_side_parent = cert;
  J->arm64_side_parent.exitno--;
  assert(lj_trace_arm64_side_parent_revalidate(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_RETRY);
  J->arm64_side_parent = cert;
  J->arm64_side_parent.child++;
  assert(lj_trace_arm64_side_parent_revalidate(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_RETRY);
  J->arm64_side_parent = cert;

  /* The destination generation is part of the publication certificate. Even
  ** a byte-identical vector replacement is an ABA change and must be rejected. */
  f->replacement_tracev = f->tracev;
  tracevec_rel(J, (TraceVec *)&f->replacement_tracev);
  assert(lj_trace_arm64_side_parent_revalidate(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_RETRY);
  tracevec_rel(J, (TraceVec *)&f->tracev);
  assert(lj_trace_arm64_side_parent_revalidate(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_OK);

  setgcrefrel(f->tracev.slot[SIDE_META_CHILD], NULL);
  assert(lj_trace_arm64_side_parent_revalidate(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_RETRY);
  setgcrefrel(f->tracev.slot[SIDE_META_CHILD],
              (const GCobj *)LJ_TRACE_PENDING);
  assert(lj_trace_arm64_side_parent_revalidate(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_OK);

  /* Slot replacement by an otherwise identical allocation is an identity
  ** change, even if its raw mcode and all semantic fields match. */
  replacement = *T;
#if LJ_ABI_PAUTH
  la_storefunc_rel(&replacement.mcauth, lj_ptr_sign(
    ptrauth_nop_cast(ASMFunction, replacement.mcode), &replacement));
#endif
  setgcrefrel(f->tracev.slot[SIDE_META_PARENT], obj2gco(&replacement));
  assert(lj_trace_arm64_side_parent_revalidate(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_RETRY);
  setgcrefrel(f->tracev.slot[SIDE_META_PARENT], obj2gco(T));
  assert(lj_trace_arm64_side_parent_revalidate(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_OK);

  setgcrefrel(f->tracev.slot[SIDE_META_PARENT], NULL);
  before = J->arm64_side_parent;
  assert(lj_trace_arm64_side_parent_revalidate(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_RETRY);
  assert(side_parent_cert_equal(&J->arm64_side_parent, &before));
  setgcrefrel(f->tracev.slot[SIDE_META_PARENT], obj2gco(T));

  memcpy(saved_footer, &f->snapmap[footer], sizeof(saved_footer));
  side_meta_pack_pc(&f->snapmap[footer], continuation+1, 0);
  assert(lj_trace_arm64_side_parent_revalidate(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_RETRY);
  memcpy(&f->snapmap[footer], saved_footer, sizeof(saved_footer));

  bc_publish((const uint32_t *)continuation,
             saved_continuationins ^ UINT32_C(0x00000100));
  assert(lj_trace_arm64_side_parent_revalidate(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_RETRY);
  bc_publish((const uint32_t *)continuation, saved_continuationins);
  assert(lj_trace_arm64_side_parent_revalidate(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_OK);

  {
    MCode *saved_mcode = T->mcode;
    T->mcode = saved_mcode+1;
    assert(lj_trace_arm64_side_parent_revalidate(J) ==
           LJ_TRACE_ARM64_SIDE_PARENT_RETRY);
    T->mcode = saved_mcode;
  }
#if LJ_ABI_PAUTH
  {
    GCtrace wrong_discriminator;
    ASMFunction saved_mcauth = trace_mcauth_acq(T);
    memset(&wrong_discriminator, 0, sizeof(wrong_discriminator));
    la_storefunc_rel(&T->mcauth, NULL);
    assert(lj_trace_arm64_side_parent_revalidate(J) ==
           LJ_TRACE_ARM64_SIDE_PARENT_RETRY);
    la_storefunc_rel(&T->mcauth, saved_mcauth);
    assert(lj_trace_arm64_side_parent_revalidate(J) ==
           LJ_TRACE_ARM64_SIDE_PARENT_OK);
    la_storefunc_rel(&T->mcauth,
      ptrauth_nop_cast(ASMFunction, T->mcode));
    assert(lj_trace_arm64_side_parent_revalidate(J) ==
           LJ_TRACE_ARM64_SIDE_PARENT_RETRY);
    la_storefunc_rel(&T->mcauth, saved_mcauth);
    assert(lj_trace_arm64_side_parent_revalidate(J) ==
           LJ_TRACE_ARM64_SIDE_PARENT_OK);
    la_storefunc_rel(&T->mcauth, lj_ptr_sign(
      ptrauth_nop_cast(ASMFunction, T->mcode), NULL));
    assert(lj_trace_arm64_side_parent_revalidate(J) ==
           LJ_TRACE_ARM64_SIDE_PARENT_RETRY);
    la_storefunc_rel(&T->mcauth, saved_mcauth);
    assert(lj_trace_arm64_side_parent_revalidate(J) ==
           LJ_TRACE_ARM64_SIDE_PARENT_OK);
    la_storefunc_rel(&T->mcauth, lj_ptr_sign(
      ptrauth_nop_cast(ASMFunction, T->mcode), &wrong_discriminator));
    assert(lj_trace_arm64_side_parent_revalidate(J) ==
           LJ_TRACE_ARM64_SIDE_PARENT_RETRY);
    la_storefunc_rel(&T->mcauth, saved_mcauth);
    assert(lj_trace_arm64_side_parent_revalidate(J) ==
           LJ_TRACE_ARM64_SIDE_PARENT_OK);
  }
#endif
  assert(lj_trace_arm64_side_parent_revalidate(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_OK);

  /* Exact ASM owner/state is part of both operations and is checked again
  ** after metadata capture to observe an asynchronous ACTIVE clear. */
  lj_trace_state_abort(J);
  assert(lj_trace_arm64_side_parent_revalidate(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_RETRY);
  lj_trace_state_store(J, LJ_TRACE_ASM);
  jit_owner_l_rel(J, NULL);
  assert(lj_trace_arm64_side_parent_revalidate(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_RETRY);
  jit_owner_l_rel(J, L);
  lj_jit_token_release_l(L, J);
  assert(lj_trace_arm64_side_parent_revalidate(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_RETRY);
  assert(lj_jit_token_try_l(L, J));
  jit_owner_l_rel(J, L);
  assert(lj_trace_arm64_side_parent_revalidate(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_OK);

  /* Closed SMR is a distinct bounded outcome and leaks no reader. Revalidate
  ** preserves the existing cert, while an authorized fresh capture clears it
  ** before reporting the failed admission. */
  assert(gc2_smr_readers_acq(g) == 0);
  expect = LJ_GC2_SMR_OPEN;
  assert(gc2_smr_reclaiming_cas(
    g, &expect, LJ_GC2_SMR_META_EXCLUSIVE));
  before = J->arm64_side_parent;
  assert(lj_trace_arm64_side_parent_revalidate(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_SMR_RETRY);
  assert(side_parent_cert_equal(&J->arm64_side_parent, &before));
  assert(gc2_smr_readers_acq(g) == 0);
  assert(lj_trace_arm64_side_parent_capture(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_SMR_RETRY);
  assert(side_parent_cert_empty(&J->arm64_side_parent));
  assert(gc2_smr_readers_acq(g) == 0);
  gc2_smr_reclaiming_rel(g, LJ_GC2_SMR_OPEN);

  assert(lj_trace_arm64_side_parent_capture(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_OK);
  assert(gc2_smr_readers_acq(g) == 0);
  setgcrefrel(f->tracev.slot[SIDE_META_PARENT], NULL);
  assert(lj_trace_arm64_side_parent_capture(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_RETRY);
  assert(side_parent_cert_empty(&J->arm64_side_parent));
  assert(gc2_smr_readers_acq(g) == 0);
  setgcrefrel(f->tracev.slot[SIDE_META_PARENT], obj2gco(T));
  assert(lj_trace_arm64_side_parent_capture(J) ==
         LJ_TRACE_ARM64_SIDE_PARENT_OK);
  assert(gc2_smr_readers_acq(g) == 0);

  lj_trace_arm64_side_parent_clear(J);
  assert(side_parent_cert_empty(&J->arm64_side_parent));
  assert(gc2_smr_readers_acq(g) == 0);
  lj_trace_state_store(J, LJ_TRACE_IDLE);
  lj_jit_token_release_l(L, J);
  assert(jit_token_acq(g) == 0 && jit_owner_l_acq(J) == NULL);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  SideMetaFixture fixture;
  assert(L != NULL);
  luaL_openlibs(L);
  run_lua(L,
    "jit.off()\n"
    "function __arm64_side_meta_f(n, bias)\n"
    "  local i = 0\n"
    "  while i < n do\n"
    "    i = i + 1\n"
    "    if bias ~= 0 then i = i + 1 end\n"
    "  end\n"
    "  return i\n"
    "end\n");
  side_meta_install(L, &fixture);
  test_second_descriptor(L, &fixture);
  test_third_descriptor(L, &fixture);
  test_metadata_mutations(L, &fixture);
  test_context_mutations(L, &fixture);
  test_parent_lifetime_certificate(L, &fixture);
  side_meta_remove(L, &fixture);
  lua_close(L);
  puts("t-arm64-jit-side-ingress-metadata OK: exact first-level LOOP metadata, idle/claim/owner contexts and parent lifetime/authentication verified");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-side-ingress-metadata SKIP");
  return 0;
}

#endif
