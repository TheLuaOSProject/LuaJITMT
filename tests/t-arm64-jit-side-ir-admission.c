/*
** Pure synthetic contract for the first bounded ARM64 side-trace grammar.
** The production side recorder remains closed; no generated code runs here.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL)

#include "lj_obj.h"
#include "lj_bc.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_target.h"
#include "lj_asm.h"

#if !LJ_HASJIT || !LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED
#error "side admission fixture requires the production ARM64 side gate closed"
#endif

enum {
  K_ONE = REF_TRUE-1u,
  R_PARENT = REF_BASE+1u,
  R_VALUE = REF_BASE+2u,
  R_ADD = REF_BASE+3u,
  R_LIMIT = REF_BASE+4u,
  R_GT = REF_BASE+5u,
  R_XPOLL = REF_BASE+6u,
  R_END = REF_BASE+7u,
  SIDE_IR_CAP = REF_BASE+16u
};

typedef struct SideFixture {
  IRIns ir[SIDE_IR_CAP];
  SnapShot snap[4];
  SnapEntry snapmap[13];
  BCIns proto[19];
  LJArm64SideIRView view;
  LJArm64SidePostRAView postra;
} SideFixture;

static SideFixture fx;

static void setir(IRRef ref, IROp op, IRType type, IRRef op1, IRRef op2)
{
  IRIns *ir = &fx.ir[ref];
  memset(ir, 0, sizeof(*ir));
  ir->op1 = (IRRef1)op1;
  ir->op2 = (IRRef1)op2;
  ir->t.irt = (uint8_t)type;
  ir->o = (IROp1)op;
}

static void set_footer(MSize snapno, MSize pcpos, uint8_t basedelta)
{
  SnapShot *snap = &fx.snap[snapno];
  uint64_t pcbase =
    ((uint64_t)(uintptr_t)(const void *)&fx.proto[pcpos] << 8) | basedelta;
  memcpy(&fx.snapmap[snap->mapofs+snap->nent], &pcbase, sizeof(pcbase));
}

static void make_semantic(void)
{
  static const IRRef snaprefs[4] = { R_VALUE, R_LIMIT, R_GT, R_XPOLL };
  static const uint32_t mapofs[4] = { 0, 3, 7, 10 };
  static const uint8_t nent[4] = { 1, 2, 1, 1 };
  static const uint8_t nslots[4] = { 5, 6, 5, 5 };
  static const uint8_t count[4] = { 0, 0, 0, SNAPCOUNT_DONE };
  static const MSize pcpos[4] = { 13, 3, 17, 7 };
  MSize i;

  memset(&fx, 0, sizeof(fx));
  setir(K_ONE, IR_KINT, IRT_INT, 1, 0);
  setir(REF_TRUE, IR_KPRI, IRT_TRUE, 0, 0);
  setir(REF_FALSE, IR_KPRI, IRT_FALSE, 0, 0);
  setir(REF_NIL, IR_KPRI, IRT_NIL, 0, 0);
  setir(REF_BASE, IR_BASE, IRT_PGC, 1, 2);
  setir(R_PARENT, IR_SLOAD, IRT_INT, 4,
	IRSLOAD_PARENT|IRSLOAD_INHERIT);
  setir(R_VALUE, IR_SLOAD, IRT_INT|IRT_GUARD, 5,
	IRSLOAD_TYPECHECK);
  setir(R_ADD, IR_ADDOV, IRT_INT|IRT_GUARD, R_VALUE, K_ONE);
  setir(R_LIMIT, IR_SLOAD, IRT_INT|IRT_GUARD, 2,
	IRSLOAD_TYPECHECK);
  setir(R_GT, IR_GT, IRT_INT|IRT_GUARD, R_LIMIT, R_ADD);
  setir(R_XPOLL, IR_XPOLL, IRT_NIL|IRT_GUARD, 1, 0);

  for (i = 0; i < 4; i++) {
    fx.snap[i].ref = (IRRef1)snaprefs[i];
    fx.snap[i].mapofs = mapofs[i];
    fx.snap[i].nslots = nslots[i];
    fx.snap[i].topslot = 5;
    fx.snap[i].nent = nent[i];
    fx.snap[i].count = count[i];
  }
  fx.snapmap[0] = SNAP(4, 0, R_PARENT);
  fx.snapmap[3] = SNAP(4, 0, R_ADD);
  fx.snapmap[4] = SNAP(5, 0, R_ADD);
  fx.snapmap[7] = SNAP(4, 0, R_ADD);
  fx.snapmap[10] = SNAP(4, 0, R_ADD);
  for (i = 0; i < 4; i++)
    set_footer(i, pcpos[i], 0);

  fx.view.ir = fx.ir;
  fx.view.snap = fx.snap;
  fx.view.snapmap = fx.snapmap;
  fx.view.proto_bc = fx.proto;
  fx.view.nins = R_END;
  fx.view.nk = K_ONE;
  fx.view.nsnap = 4;
  fx.view.nsnapmap = 13;
  fx.view.proto_sizebc = 19;
  fx.view.baseslot = 1+LJ_FR2;
  fx.view.root_topslot = 5;
  fx.view.traceno = 2;
  fx.view.parent = 1;
  fx.view.root = 1;
  fx.view.link = 1;
  fx.view.exitno = 2;
  fx.view.startins = BCINS_AD(BC_JMP, 0, 0);
  fx.view.linktype = LJ_TRLINK_ROOT;
  fx.view.sinktags = 0;
  fx.view.base_delta = 0;
}

static void expect_semantic(int admitted)
{
  LJArm64IRReject reject;
  int result = lj_asm_arm64_side_ir_admit(&fx.view, &reject);
  if (result != admitted)
    fprintf(stderr, "semantic result=%d wanted=%d reason=%d ref=%u op=%u detail=%u\n",
	result, admitted, (int)reject.reason, (unsigned)reject.ref,
	(unsigned)reject.op, (unsigned)reject.detail);
  assert(result == admitted);
  assert(admitted ? reject.reason == LJ_ARM64_IR_REJECT_NONE :
	 reject.reason != LJ_ARM64_IR_REJECT_NONE);
}

#define SEMANTIC_MUTATION(stmt) \
  do { make_semantic(); stmt; expect_semantic(0); } while (0)

static void test_semantic_header(void)
{
  LJArm64IRReject reject;
  make_semantic();
  expect_semantic(1);
  assert(!lj_asm_arm64_side_ir_admit(NULL, &reject));

  /* Trace numbers are relational, not hard-coded to the initial 1/2 slots. */
  make_semantic();
  fx.view.traceno = 9;
  fx.view.parent = fx.view.root = fx.view.link = 7;
  fx.ir[REF_BASE].op1 = 7;
  expect_semantic(1);

  make_semantic();
  fx.view.traceno = UINT16_MAX;
  fx.view.parent = fx.view.root = fx.view.link = UINT16_MAX-1u;
  fx.ir[REF_BASE].op1 = UINT16_MAX-1u;
  expect_semantic(1);

  SEMANTIC_MUTATION(fx.view.ir = NULL);
  SEMANTIC_MUTATION(fx.view.snap = NULL);
  SEMANTIC_MUTATION(fx.view.snapmap = NULL);
  SEMANTIC_MUTATION(fx.view.proto_bc = NULL);
  SEMANTIC_MUTATION(fx.view.nins--);
  SEMANTIC_MUTATION(fx.view.nins++);
  SEMANTIC_MUTATION(fx.view.nk--);
  SEMANTIC_MUTATION(fx.view.nk++);
  SEMANTIC_MUTATION(fx.view.nsnap--);
  SEMANTIC_MUTATION(fx.view.nsnapmap--);
  SEMANTIC_MUTATION(fx.view.proto_sizebc--);
  SEMANTIC_MUTATION(fx.view.proto_sizebc++);
  SEMANTIC_MUTATION(fx.view.baseslot++);
  SEMANTIC_MUTATION(fx.view.root_topslot--);
  SEMANTIC_MUTATION(fx.view.traceno = 0);
  SEMANTIC_MUTATION(fx.view.traceno = UINT16_MAX+1u);
  SEMANTIC_MUTATION(fx.view.parent = 0);
  SEMANTIC_MUTATION(fx.view.parent = UINT16_MAX+1u);
  SEMANTIC_MUTATION(fx.view.traceno = fx.view.parent);
  SEMANTIC_MUTATION(fx.view.root++);
  SEMANTIC_MUTATION(fx.view.link++);
  SEMANTIC_MUTATION(fx.view.exitno++);
  SEMANTIC_MUTATION(fx.view.startins = BCINS_AD(BC_JMP, 1, 0));
  SEMANTIC_MUTATION(fx.view.linktype = LJ_TRLINK_LOOP);
  SEMANTIC_MUTATION(fx.view.sinktags = 1);
  SEMANTIC_MUTATION(fx.view.base_delta = 1);
  SEMANTIC_MUTATION(fx.view.proto_bc =
	(const BCIns *)((uintptr_t)(const void *)fx.proto+1u));
  SEMANTIC_MUTATION(fx.view.proto_bc =
	(const BCIns *)(uintptr_t)(UINT64_C(1) << 60));
}

static void test_semantic_constants_and_ir(void)
{
  IRRef ref;
  static const IRRef refs[] = {
    REF_BASE, R_PARENT, R_VALUE, R_ADD, R_LIMIT, R_GT, R_XPOLL
  };
  MSize i;

  SEMANTIC_MUTATION(setir(K_ONE, IR_KINT, IRT_INT, 2, 0));
  SEMANTIC_MUTATION(setir(K_ONE, IR_KNUM, IRT_NUM, 1, 0));
  SEMANTIC_MUTATION(setir(K_ONE, IR_KINT, IRT_NUM, 1, 0));
  SEMANTIC_MUTATION(setir(REF_TRUE, IR_KPRI, IRT_FALSE, 0, 0));
  SEMANTIC_MUTATION(setir(REF_FALSE, IR_KINT, IRT_INT, 0, 0));
  SEMANTIC_MUTATION(fx.ir[REF_NIL].op1 = 1);

  for (i = 0; i < sizeof(refs)/sizeof(refs[0]); i++) {
    ref = refs[i];
    make_semantic();
    fx.ir[ref].o = IR_NOP;
    expect_semantic(0);
    make_semantic();
    fx.ir[ref].t.irt ^= IRT_GUARD;
    expect_semantic(0);
    make_semantic();
    fx.ir[ref].op1++;
    expect_semantic(0);
    make_semantic();
    fx.ir[ref].op2++;
    expect_semantic(0);
  }
  SEMANTIC_MUTATION(fx.ir[R_PARENT].op2 = IRSLOAD_PARENT);
  SEMANTIC_MUTATION(fx.ir[R_PARENT].t.irt |= IRT_GUARD);
  SEMANTIC_MUTATION(fx.ir[R_VALUE].op2 |= IRSLOAD_PARENT);
  SEMANTIC_MUTATION(fx.ir[R_ADD].op2 = R_PARENT);
  SEMANTIC_MUTATION(fx.ir[R_LIMIT].op1 = 3);
  SEMANTIC_MUTATION(fx.ir[R_GT].op1 = R_PARENT);
  SEMANTIC_MUTATION(fx.ir[R_GT].op2 = R_PARENT);
  SEMANTIC_MUTATION(fx.ir[R_XPOLL].op1 = 0);

  /* BASE operands follow the relational parent/exit fields. */
  SEMANTIC_MUTATION(fx.ir[REF_BASE].op1 = 2);
  SEMANTIC_MUTATION(fx.ir[REF_BASE].op2 = 3);
}

static void test_semantic_snapshots(void)
{
  static const MSize pcpos[4] = { 13, 3, 17, 7 };
  MSize i;

  /* Exit hotness/count is mutable and outside this immutable certificate. */
  make_semantic();
  for (i = 0; i < 4; i++)
    fx.snap[i].count = (uint8_t)(31u+i*37u);
  expect_semantic(1);
  make_semantic();
  for (i = 0; i < 4; i++)
    fx.snap[i].mcofs = (uint16_t)(100u+i);
  expect_semantic(1);

  for (i = 0; i < 4; i++) {
    make_semantic(); fx.snap[i].ref++; expect_semantic(0);
    make_semantic(); fx.snap[i].mapofs++; expect_semantic(0);
    make_semantic(); fx.snap[i].nent++; expect_semantic(0);
    make_semantic(); fx.snap[i].nslots++; expect_semantic(0);
    make_semantic(); fx.snap[i].topslot--; expect_semantic(0);
    make_semantic(); set_footer(i, pcpos[i], 1); expect_semantic(0);
    make_semantic(); set_footer(i, (pcpos[i]+1u)%19u, 0); expect_semantic(0);
  }
  SEMANTIC_MUTATION(fx.snapmap[0] = SNAP(3, 0, R_PARENT));
  SEMANTIC_MUTATION(fx.snapmap[0] = SNAP(4, SNAP_NORESTORE, R_PARENT));
  SEMANTIC_MUTATION(fx.snapmap[0] = SNAP(4, 0, R_ADD));
  SEMANTIC_MUTATION(fx.snapmap[3] = SNAP(4, 0, R_PARENT));
  SEMANTIC_MUTATION(fx.snapmap[4] = SNAP(5, 0, R_VALUE));
  SEMANTIC_MUTATION(fx.snapmap[7] = SNAP(4, SNAP_FRAME, R_ADD));
  SEMANTIC_MUTATION(fx.snapmap[10] = SNAP(5, 0, R_ADD));
}

static void make_postra(void)
{
  IRRef ref;
  make_semantic();
  setir(R_END, IR_NOP, IRT_NIL, 0, 0);
  for (ref = K_ONE; ref <= REF_NIL; ref++)
    fx.ir[ref].prev = REGSP_INIT;
  fx.ir[REF_BASE].r = RID_BASE;
  fx.ir[REF_BASE].s = SPS_NONE;
  fx.ir[R_PARENT].r = RID_X4;
  fx.ir[R_VALUE].r = RID_X5;
  fx.ir[R_ADD].r = RID_X6;
  fx.ir[R_LIMIT].r = RID_X7;
  fx.ir[R_GT].r = RID_INIT;
  fx.ir[R_XPOLL].r = RID_INIT;

  fx.postra.semantic = fx.view;
  fx.postra.nins = R_END+1u;
  fx.postra.spadjust = 0;
  fx.postra.parent_spadjust = 0;
  fx.postra.topslot = 5;
  fx.postra.parent_topslot = 5;
  fx.postra.parent_slot4 = REGSP(RID_X4, SPS_NONE);
}

static void expect_postra(int admitted)
{
  IRRef semantic_nins = 0;
  int result = lj_asm_arm64_side_postra_admit(&fx.postra, &semantic_nins);
  if (result != admitted)
    fprintf(stderr, "postra result=%d wanted=%d\n", result, admitted);
  assert(result == admitted);
  if (admitted)
    assert(semantic_nins == R_END);
}

#define POSTRA_MUTATION(stmt) \
  do { make_postra(); stmt; expect_postra(0); } while (0)

static void test_postra(void)
{
  IRRef ref;
  make_postra();
  expect_postra(1);
  assert(!lj_asm_arm64_side_postra_admit(NULL, NULL));

  make_postra();
  fx.snap[0].count = SNAPCOUNT_DONE;
  fx.snap[3].count = 0;
  expect_postra(1);

  POSTRA_MUTATION(fx.postra.semantic.ir = NULL);
  POSTRA_MUTATION(fx.postra.nins--);
  POSTRA_MUTATION(fx.postra.nins++);
  POSTRA_MUTATION(fx.postra.spadjust = 16);
  POSTRA_MUTATION(fx.postra.parent_spadjust = 16);
  POSTRA_MUTATION(fx.postra.topslot = 6);
  POSTRA_MUTATION(fx.postra.parent_topslot = 6);
  POSTRA_MUTATION(setir(R_END, IR_RENAME, IRT_NIL, R_ADD, 0));
  POSTRA_MUTATION(fx.ir[R_END].t.irt = IRT_INT);
  POSTRA_MUTATION(fx.ir[R_END].op1 = 1);
  POSTRA_MUTATION(fx.ir[R_END].op2 = 1);
  POSTRA_MUTATION(fx.ir[R_END].prev = REGSP_INIT);

  POSTRA_MUTATION(fx.postra.parent_slot4 = REGSP_INIT);
  POSTRA_MUTATION(fx.postra.parent_slot4 = REGSP(RID_X4, 2));
  POSTRA_MUTATION(fx.postra.parent_slot4 = REGSP(RID_D0, 0));
  POSTRA_MUTATION(fx.ir[REF_BASE].r = RID_X0);
  POSTRA_MUTATION(fx.ir[REF_BASE].s = 2);
  POSTRA_MUTATION(fx.ir[R_PARENT].r = RID_X3);

  for (ref = R_PARENT; ref <= R_LIMIT; ref++) {
    make_postra(); fx.ir[ref].r = RID_INIT; expect_postra(0);
    make_postra(); fx.ir[ref].r = RID_D0; expect_postra(0);
    make_postra(); fx.ir[ref].s = 2; expect_postra(0);
  }
  POSTRA_MUTATION(fx.ir[R_GT].r = RID_X0);
  POSTRA_MUTATION(fx.ir[R_GT].s = 2);
  POSTRA_MUTATION(fx.ir[R_XPOLL].r = RID_X0);
  POSTRA_MUTATION(fx.ir[R_XPOLL].s = 2);

  for (ref = K_ONE; ref <= REF_NIL; ref++) {
    make_postra(); fx.ir[ref].prev = 0; expect_postra(0);
  }
  POSTRA_MUTATION(fx.ir[R_ADD].o = IR_TNEW);
  POSTRA_MUTATION(fx.snap[1].nslots = 5);
}

int main(void)
{
  LJ_STATIC_ASSERT(TRACE_ARM64_INT_SIDE_ADMITTED == 0x80);
  LJ_STATIC_ASSERT((TRACE_ARM64_INT_SIDE_ADMITTED &
	(TRACE_ARM64_INT_LOOP_ADMITTED|TRACE_ARM64_INT_FORL_ADMITTED|
	 TRACE_ARM64_TRUE_FUNCF_ADMITTED|TRACE_ENTRY_GATED|
	 TRACE_EXITTAB_MCODE|TRACE_RETIRED_UNPUBLISHED)) == 0);
  test_semantic_header();
  test_semantic_constants_and_ir();
  test_semantic_snapshots();
  test_postra();
  puts("t-arm64-jit-side-ir-admission OK");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-side-ir-admission SKIP");
  return 0;
}

#endif
