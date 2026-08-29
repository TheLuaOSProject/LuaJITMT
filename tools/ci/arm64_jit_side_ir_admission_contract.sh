#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_side_ir_admission_contract SKIP: requires native macOS arm64"
  exit 0
fi

if test -z "${SDKROOT:-}"; then
  SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
  export SDKROOT
fi

cc=${CC:-$(xcrun --sdk macosx --find clang)}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
archive=$root/src/libluajit.a
asm_source=$root/src/lj_asm.c
admit_source=$root/src/lj_asm_arm64_admit.h
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-side-ir.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

fixture=$tmpdir/t-arm64-jit-side-ir-admission
arm64e_fixture=$tmpdir/t-arm64e-jit-side-ir-admission
audit_object=$tmpdir/lj_asm-arm64e.o
pure_region=$tmpdir/pure-side-region.txt
shape_region=$tmpdir/side-shape-region.txt
second_postra_region=$tmpdir/side-second-postra-region.txt
third_postra_region=$tmpdir/side-third-postra-region.txt
return_negative_region=$tmpdir/side-return-negative-region.txt
trace_asm=$tmpdir/trace-asm.txt
head_side=$tmpdir/head-side.txt
tail_side=$tmpdir/tail-side.txt
post_snap=$tmpdir/post-snap.txt
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT'
arm64e_xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR"

require_order()
{
  region=$1
  before=$2
  after=$3
  label=$4
  before_line=$(awk -v needle="$before" 'index($0, needle) { print NR; exit }' \
    "$region")
  after_line=$(awk -v needle="$after" 'index($0, needle) { print NR; exit }' \
    "$region")
  if test -z "$before_line" || test -z "$after_line" || \
     test "$before_line" -ge "$after_line"; then
    echo "ARM64 side assembler ordering changed: $label" >&2
    exit 1
  fi
}

test -f "$archive" || {
  echo "ARM64 side IR contract requires an existing experimental build" >&2
  exit 1
}

nm "$archive" | grep ' T _lj_asm_arm64_side_ir_admit$' >/dev/null || {
  echo "experimental archive lacks the pure ARM64 side semantic gate" >&2
  exit 1
}
nm "$archive" | grep ' T _lj_asm_arm64_side_prehead_admit$' >/dev/null || {
  echo "experimental archive lacks the pure ARM64 side pre-head gate" >&2
  exit 1
}
nm "$archive" | grep ' T _lj_asm_arm64_side_postra_admit$' >/dev/null || {
  echo "experimental archive lacks the pure ARM64 side post-RA gate" >&2
  exit 1
}

# Production assembly consumes the exact semantic, parent-lifetime and post-RA
# certificates for the bounded first-side canary. The broad side gate stays
# closed for every unsupported first side and side-of-side.
grep -E '^#define LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED[[:space:]]+1$' \
  "$root/src/lj_arch.h" >/dev/null
grep -E '^#define LJ_ARM64_JIT_FIRST_SIDE_RECORDER_FAIL_CLOSED[[:space:]]+0$' \
  "$root/src/lj_arch.h" >/dev/null
grep -F '#define TRACE_ARM64_INT_SIDE_ADMITTED' "$root/src/lj_jit.h" | \
  grep -F '0x80' >/dev/null
grep -F 'lj_asm_arm64_side_ir_admit' "$root/src/lj_asm.h" >/dev/null
for required in \
  'enum { LJ_ARM64_SIDE_CHILD_NSNAP = 5 };' \
  'typedef struct LJArm64SideShape {' \
  'ExitNo exitno;' \
  'MSize parent_nsnap;' \
  'MSize continuation_pc;' \
  'MSize child_pcpos[LJ_ARM64_SIDE_CHILD_NSNAP];' \
  'uint32_t inherited_reg;' \
  'uint32_t sload_reg;' \
  'int32_t addends[2];  /* Repeat addends[0] for a singleton exact set. */' \
  'lj_asm_arm64_side_shape(ExitNo exitno);'; do
  grep -F "$required" "$root/src/lj_asm.h" >/dev/null || {
    echo "ARM64 side descriptor schema changed: $required" >&2
    exit 1
  }
done
grep -F 'lj_asm_arm64_side_prehead_admit' "$root/src/lj_asm.h" >/dev/null
grep -F 'lj_asm_arm64_side_postra_admit' "$root/src/lj_asm.h" >/dev/null
grep -F 'sh "$root/tools/ci/arm64_jit_side_ir_admission_contract.sh"' \
  "$root/tools/ci/arm64_jit_fail_closed_gate.sh" >/dev/null
if grep -F 'LUAJIT_MT_ARM64_SIDE_POSTRA_OBSERVE' \
     "$root/src/lj_arch.h" "$root/src/lj_trace.c" "$root/src/lj_trace.h" \
     "$asm_source" "$admit_source" "$root/src/lj_asm.h" >/dev/null; then
  echo "disposable ARM64 side observation overlay leaked into production sources" >&2
  exit 1
fi

awk '/^\/\* -- Pure ARM64 first-side admission/ { copy=1 }
     copy { print }
     copy && /^static int arm64_ir_funcf_snapshots/ { exit }' \
  "$admit_source" >"$pure_region"
test -s "$pure_region"
sed -n '/^const LJArm64SideShape \*lj_asm_arm64_side_shape(/,/^}/p' \
  "$admit_source" >"$shape_region"
test -s "$shape_region"
for required in \
  'static const LJArm64SideShape shapes[] = {' \
  '{ 2u, 8u, 13u, { 13u, 14u, 3u, 17u, 7u },' \
  'RID_X28, RID_X27, { 1, 1 } },' \
  '{ 6u, 9u, 10u, { 10u, 11u, 3u, 17u, 7u },' \
  'RID_X27, RID_X28, { 1, 2 } },' \
  '{ 7u, 11u, 13u, { 13u, 14u, 3u, 17u, 7u },' \
  'RID_X28, RID_X27, { 1, 1 } }' \
  'if (shapes[i].exitno == exitno)' \
  'return &shapes[i];' \
  'return NULL;'; do
  grep -F "$required" "$shape_region" >/dev/null || {
    echo "ARM64 side descriptor table changed: $required" >&2
    exit 1
  }
done
test "$(grep -Ec '^    \{ [0-9]+u, [0-9]+u, [0-9]+u, \{' \
  "$shape_region")" = 3 || {
  echo "ARM64 side descriptor table is no longer exactly three rows" >&2
  exit 1
}

for required in \
  'ARM64_SIDE_K_ADDEND = REF_TRUE-1u' \
  'ARM64_SIDE_SEMANTIC_NINS = REF_BASE+7u' \
  'ARM64_SIDE_R_CGET = REF_BASE+2u' \
  'static const IRRef snaprefs[LJ_ARM64_SIDE_CHILD_NSNAP]' \
  'ARM64_SIDE_R_CGET, ARM64_SIDE_R_ADD, ARM64_SIDE_R_LIMIT' \
  'static const MSize mapofs[LJ_ARM64_SIDE_CHILD_NSNAP]' \
  '{ 0, 3, 7, 11, 14 };' \
  'static const uint8_t nent[LJ_ARM64_SIDE_CHILD_NSNAP]' \
  '{ 1, 2, 2, 1, 1 };' \
  'static const uint8_t nslots[LJ_ARM64_SIDE_CHILD_NSNAP]' \
  '{ 5, 6, 6, 5, 5 };' \
  'const LJArm64SideShape *shape;' \
  'shape = lj_asm_arm64_side_shape(view->exitno);' \
  'shape == NULL' \
  'view->proto_sizebc != 19u' \
  'view->nk != ARM64_SIDE_K_ADDEND || view->nsnap != 5u' \
  'view->nsnapmap != 17u' \
  'view->traceno == 0' \
  'view->traceno > UINT16_MAX || view->parent == 0' \
  'view->parent > UINT16_MAX || view->traceno == view->parent' \
  'view->root != view->parent' \
  'view->link != view->parent' \
  'view->startins != BCINS_AD(BC_JMP, 0, 0)' \
  'view->linktype != LJ_TRLINK_ROOT' \
  'ins.o != IR_KINT || ins.t.irt != IRT_INT ||' \
  '(ins.i != shape->addends[0] && ins.i != shape->addends[1])' \
  'ARM64_SIDE_REQUIRE(REF_BASE, IR_BASE, IRT_PGC,' \
  'view->parent, view->exitno);' \
  'IRSLOAD_PARENT|IRSLOAD_INHERIT' \
  'ARM64_SIDE_R_CGET, IR_NOP, IRT_NIL, 0, 0' \
  'ARM64_SIDE_R_ADD, IR_ADDOV, IRT_INT|IRT_GUARD' \
  'ARM64_SIDE_R_PARENT, ARM64_SIDE_K_ADDEND);' \
  'ARM64_SIDE_R_GT, IR_GT, IRT_INT|IRT_GUARD' \
  'ARM64_SIDE_R_LIMIT, ARM64_SIDE_R_ADD);' \
  'ARM64_SIDE_R_XPOLL, IR_XPOLL, IRT_NIL|IRT_GUARD, 1, 0' \
  'expected > (uintptr_t)(UINT64_MAX >> 8)' \
  'for (snapno = 0; snapno < LJ_ARM64_SIDE_CHILD_NSNAP; snapno++)' \
  'snapno+1u < LJ_ARM64_SIDE_CHILD_NSNAP ?' \
  'view, snapno, shape->child_pcpos[snapno]))' \
  '&view->snapmap[8]' \
  '&view->snapmap[11]' \
  '&view->snapmap[14]' \
  'SNAP(4, 0, ARM64_SIDE_R_PARENT)' \
  'SNAP(5, 0, ARM64_SIDE_R_ADD)' \
  'int lj_asm_arm64_side_prehead_admit(' \
  'Reg valueregs[4];' \
  'shape = lj_asm_arm64_side_shape(view->semantic.exitno);' \
  'valueregs[0] = shape->sload_reg;' \
  'valueregs[1] = RID_INIT;' \
  'valueregs[2] = shape->inherited_reg;' \
  'valueregs[3] = shape->sload_reg;' \
  'view->nins != ARM64_SIDE_SEMANTIC_NINS+1u' \
  'view->stopins != ARM64_SIDE_R_PARENT' \
  'view->orignins != ARM64_SIDE_SEMANTIC_NINS' \
  'view->spadjust != 0 || view->parent_spadjust != 0' \
  'view->parentmap == NULL || view->parentmap_n != 1u' \
  'view->branch_track != (uint8_t)LJ_ABI_BRANCH_TRACK' \
  'headidx = (MSize)LJ_ABI_BRANCH_TRACK' \
  'view->entry_words < headidx+4u' \
  'view->entry[0] != A64I_LE(A64I_BTI_J)' \
  'view->entry[headidx] != A64I_LE(A64I_MOVx |' \
  'A64F_D(shape->sload_reg)' \
  'A64F_M(shape->inherited_reg)' \
  'A64I_MOVZw |' \
  'A64F_U16(view->semantic.traceno)' \
  'view->entry[headidx+2u] != A64I_LE(A64I_DMB_ISH)' \
  'A64I_STRw | A64F_D(RID_TMP) | A64F_N(RID_DISPATCH)' \
  'A64F_U12((uint32_t)DISPATCH_TG(vmstate) >> 2)' \
  'view->entry[headidx+3u] != A64I_LE(vmstore)' \
  'ins.o != IR_NOP || ins.t.irt != IRT_NIL' \
  'ins.op1 != 0 || ins.op2 != 0 || ins.r != 0 || ins.s != 0' \
  'view->parentmap[0] != REGSP(shape->inherited_reg, SPS_NONE)' \
  'ins.r != RID_BASE || ins.s != SPS_NONE' \
  'ins.r != valueregs[ref-ARM64_SIDE_R_PARENT]' \
  'ins.r != RID_INIT || ins.s != SPS_NONE' \
  '!lj_asm_arm64_side_prehead_admit(view, &semantic_nins)'; do
  grep -F "$required" "$pure_region" >/dev/null || {
    echo "ARM64 side certificate invariant changed: $required" >&2
    exit 1
  }
done

require_order "$pure_region" \
  'shape = lj_asm_arm64_side_shape(view->exitno);' \
  'shape->addends[0]' \
  'descriptor lookup before exact addend membership'
require_order "$pure_region" \
  'shape = lj_asm_arm64_side_shape(view->exitno);' \
  'shape->child_pcpos[snapno]' \
  'descriptor lookup before child snapshot geometry indexing'
require_order "$pure_region" \
  'shape = lj_asm_arm64_side_shape(view->semantic.exitno);' \
  'view->parentmap[0] != REGSP(shape->inherited_reg, SPS_NONE)' \
  'descriptor lookup before pre-head inherited-register indexing'
require_order "$pure_region" \
  '(shape = lj_asm_arm64_side_shape(view->semantic.exitno)) == NULL' \
  'A64F_D(shape->sload_reg)' \
  'descriptor lookup before post-RA head-move validation'

if grep -E 'snap_count_acq|trace_save|traceslot_publish|lj_mcode_|lj_ir_call|asm_call|lj_trace_err|trace_exittarget' \
     "$pure_region" >/dev/null; then
  echo "pure ARM64 side certificate gained mutable-count or publication effects" >&2
  exit 1
fi
awk '/^void lj_asm_trace\(/ { copy=1 }
     copy { print }
     copy && /^#if LJ_TARGET_ARM64 && defined\(LJ_ARM64_EMIT_TEST_HELPERS\)/ {
       exit
     }' "$asm_source" >"$trace_asm"
sed -n '/^static void asm_head_side(ASMState \*as)/,/^}/p' \
  "$asm_source" >"$head_side"
awk '/^  \/\* Set trace entry point before fixing up tail/ { copy=1 }
     copy { print }
     copy && /T->szmcode =/ { exit }' \
  "$asm_source" >"$tail_side"
awk '/^  T->szmcode =/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$asm_source" >"$post_snap"
test -s "$trace_asm" && test -s "$head_side" && test -s "$tail_side" && \
  test -s "$post_snap"

# Pin the bounded production-consumption boundary. The side semantic gate and
# parent capture must precede every fallible allocation. Every raw parent-body
# use is preceded by exact certificate revalidation, and only the full final
# post-RA gate can lead to the scratch admission marker.
test "$(grep -Fc 'lj_asm_arm64_side_ir_admit(' "$trace_asm")" = 1
test "$(grep -Fc 'lj_asm_arm64_side_postra_admit(' "$trace_asm")" = 1
test "$(grep -Fc 'lj_trace_arm64_side_parent_capture(J)' "$trace_asm")" = 1
test "$(grep -Fc 'TRACE_ARM64_INT_SIDE_ADMITTED' "$trace_asm")" = 1
require_order "$trace_asm" 'J->loopref != 0' \
  'lj_asm_arm64_side_ir_admit(' \
  'non-loop side shape before semantic admission'
require_order "$trace_asm" 'lj_asm_arm64_side_ir_admit(' \
  'lj_trace_arm64_side_parent_capture(J)' \
  'semantic side gate before parent capture'
require_order "$trace_asm" 'lj_trace_arm64_side_parent_capture(J)' \
  'as->orignins = lj_ir_nextins(J);' \
  'parent capture before IR growth'
require_order "$trace_asm" 'lj_trace_arm64_side_parent_capture(J)' \
  'J->curfinal = lj_trace_alloc(J->L, T);' \
  'parent capture before scratch trace allocation'
require_order "$trace_asm" 'lj_trace_arm64_side_parent_capture(J)' \
  'lj_mcode_reserve(J, &as->mcbot)' \
  'parent capture before mcode reservation'
require_order "$trace_asm" 'lj_trace_arm64_side_parent_revalidate(J)' \
  'asm_setup_regsp(as);' \
  'parent revalidation before regsp map construction'
grep -F 'as->parent = J->parent ? J->arm64_side_parent.body : NULL;' \
  "$trace_asm" >/dev/null

require_order "$head_side" 'lj_trace_arm64_side_parent_revalidate(as->J)' \
  'as->parent != as->J->arm64_side_parent.body' \
  'pre-head revalidation before identity comparison'
require_order "$head_side" 'as->parent != as->J->arm64_side_parent.body' \
  'lj_asm_arm64_side_prehead_admit(' \
  'pre-head identity before layout validation'
require_order "$head_side" 'lj_asm_arm64_side_prehead_admit(' \
  'trace_ir_acq(as->parent)' \
  'pre-head layout validation before parent IR dereference'
require_order "$head_side" 'lj_asm_arm64_side_prehead_admit(' \
  'as->parentmap[i - REF_FIRST]' \
  'pre-head layout validation before parentmap indexing'
grep -F 'view->parentmap = as->parentmap;' "$asm_source" >/dev/null
grep -F 'view->parentmap_n = as->parentmap_n;' "$asm_source" >/dev/null
grep -F 'view->nins = as->J->curfinal ? trace_nins_acq(as->J->curfinal) : 0;' \
  "$asm_source" >/dev/null

require_order "$trace_asm" 'lj_asm_arm64_side_postra_admit(' \
  'asm_snap_fixup_mcofs(as);' \
  'full side post-RA gate before last fallible fixup'
require_order "$post_snap" 'asm_snap_fixup_mcofs(as);' \
  'lj_trace_arm64_side_parent_revalidate(J)' \
  'last fallible fixup before final parent revalidation'
require_order "$post_snap" 'lj_trace_arm64_side_parent_revalidate(J)' \
  'TRACE_ARM64_INT_SIDE_ADMITTED' \
  'final parent revalidation before side admission marker'
require_order "$post_snap" 'TRACE_ARM64_INT_SIDE_ADMITTED' \
  'asm_mcode_fixup(T->mcode, T->szmcode);' \
  'side admission marker before no-throw mcode finalization'

require_order "$tail_side" 'lj_trace_arm64_side_parent_revalidate(J)' \
  'T->link != J->arm64_side_parent.parent' \
  'tail parent revalidation before link identity'
require_order "$tail_side" 'T->link != J->arm64_side_parent.parent' \
  'certified_parent_mcode = J->arm64_side_parent.mcode;' \
  'tail identity before certified target load'
require_order "$tail_side" 'certified_parent_mcode = J->arm64_side_parent.mcode;' \
  'tail_pc = asm_tail_fixup(as, T->link, certified_parent_mcode);' \
  'certified target before tail finalization'

for required in \
  'fx.view.traceno = 9;' \
  'fx.view.parent = fx.view.root = fx.view.link = 7;' \
  'fx.ir[REF_BASE].op1 = 7;' \
  'fx.view.traceno = UINT16_MAX;' \
  'fx.view.parent = fx.view.root = fx.view.link = UINT16_MAX-1u;' \
  'BCIns proto[19];' \
  'SnapShot snap[5];' \
  'SnapEntry snapmap[17];' \
  'MCode entry[5];' \
  'static const uint32_t mapofs[5] = { 0, 3, 7, 11, 14 };' \
  'static const uint8_t nent[5] = { 1, 2, 2, 1, 1 };' \
  'static const MSize pcpos[5] = { 13, 14, 3, 17, 7 };' \
  'fx.view.proto_sizebc = 19;' \
  'fx.view.traceno = fx.view.parent' \
  'fx.view.exitno++' \
  'fx.view.exitno = 6;' \
  'fx.ir[REF_BASE].op2 = 6;' \
  'set_footer(0, 10, 0);' \
  'set_footer(1, 11, 0);' \
  'fx.view.exitno = 7;' \
  'fx.ir[REF_BASE].op2 = 7;' \
  'make_semantic(); fx.view.exitno = 3; fx.ir[REF_BASE].op2 = 3;' \
  'make_semantic(); fx.view.exitno = 8; fx.ir[REF_BASE].op2 = 8;' \
  'setir(K_ADDEND, IR_KINT, IRT_INT, 2, 0)' \
  'fx.ir[K_ADDEND].i = 3;' \
  'fx.view.linktype = LJ_TRLINK_RETURN;' \
  'fx.ir[R_GT].o = IR_LE;' \
  'setir(K_ADDEND, IR_KNUM, IRT_NUM, 1, 0)' \
  'fx.ir[R_PARENT].op2 = IRSLOAD_PARENT' \
  'setir(R_CGET, IR_NOP, IRT_NIL, 0, 0)' \
  'fx.ir[ref].o == IR_NOP ? IR_XBAR : IR_NOP' \
  'fx.ir[R_CGET].t.irt = IRT_INT' \
  'fx.ir[R_ADD].op1 = R_CGET' \
  'fx.ir[R_ADD].op2 = R_PARENT' \
  'fx.ir[R_GT].op2 = R_PARENT' \
  'fx.ir[R_XPOLL].op1 = 0' \
  'fx.snap[i].count = (uint8_t)(31u+i*37u);' \
  'expect_semantic(1);' \
  'fx.snap[i].mcofs = (uint16_t)(100u+i);' \
  'fx.snap[i].ref++' \
  'fx.snap[i].mapofs++' \
  'fx.snap[i].nent++' \
  'fx.snap[i].nslots++' \
  'set_footer(i, pcpos[i], 1)' \
  'SNAP(4, SNAP_NORESTORE, R_PARENT)' \
  'fx.snapmap[3] = SNAP(4, 0, R_ADD)' \
  'fx.snapmap[4] = SNAP(5, 0, R_CGET)' \
  'fx.snapmap[8] = SNAP(5, 0, R_PARENT)' \
  'fx.postra.spadjust = 16' \
  'fx.postra.parent_spadjust = 16' \
  'setir(R_END, IR_RENAME, IRT_NIL, R_ADD, 0)' \
  'fx.postra.stopins = R_PARENT;' \
  'fx.postra.orignins = R_END;' \
  'fx.postra.parentmap_n = 1;' \
  'fx.postra.entry_words = headidx+4u;' \
  'fx.postra.branch_track = (uint8_t)LJ_ABI_BRANCH_TRACK;' \
  'fx.postra.semantic.traceno = 9;' \
  'A64F_U16(fx.postra.semantic.traceno) | A64F_D(RID_TMP)' \
  'expect_prehead(1);' \
  'fx.postra.semantic.exitno = 6;' \
  'fx.postra.semantic.exitno = 7;' \
  'make_third_postra();' \
  'lj_asm_arm64_side_prehead_admit(NULL, NULL)' \
  'PREHEAD_MUTATION(fx.postra.semantic.exitno++)' \
  'PREHEAD_MUTATION(fx.postra.parentmap = NULL)' \
  'PREHEAD_MUTATION(fx.parentmap[0] = REGSP_INIT)' \
  'PREHEAD_MUTATION(fx.ir[R_PARENT].r = RID_X0)' \
  'PREHEAD_MUTATION(fx.ir[R_CGET].r = RID_X0)' \
  'fx.postra.entry = NULL; expect_prehead(1);' \
  'fx.postra.entry_words = 0; expect_prehead(1);' \
  '(uint8_t)!LJ_ABI_BRANCH_TRACK; expect_prehead(1);' \
  'fx.entry[LJ_ABI_BRANCH_TRACK] ^= 1u; expect_prehead(1);' \
  'fx.postra.parentmap = NULL' \
  'fx.postra.parentmap_n = 2' \
  'fx.parentmap[0] = REGSP(RID_X28, 2)' \
  'fx.parentmap[0] = REGSP(RID_X27, 0)' \
  'fx.parentmap[0] = REGSP(RID_D0, 0)' \
  'fx.postra.entry = NULL' \
  'fx.postra.entry_words = 0' \
  'fx.postra.entry_words = headidx+3u' \
  'fx.postra.branch_track =' \
  'fx.entry[0] = A64I_LE(A64I_NOP)' \
  'A64I_MOVw | A64F_D(RID_X27)' \
  'A64F_M(RID_X26)' \
  'A64I_MOVZx |' \
  'A64F_U16(fx.postra.semantic.traceno+1u)' \
  'fx.entry[headidx+2u] = A64I_LE(A64I_NOP)' \
  'fx.entry[headidx+3u] = A64I_LE(A64I_LDRw |' \
  'A64F_D(RID_X0) | A64F_N(RID_DISPATCH)' \
  'A64F_D(RID_TMP) | A64F_N(RID_BASE)' \
  'A64I_LE(vmstore ^ A64F_U12(1u))' \
  'fx.ir[ref].r = RID_X0' \
  'fx.ir[ref].r = RID_D0' \
  'fx.ir[ref].s = 2' \
  'fx.ir[R_END].r = RID_X1' \
  'fx.ir[R_END].s = 2' \
  'fx.ir[R_GT].r = RID_X0' \
  'fx.ir[R_XPOLL].s = 2' \
  'TRACE_ARM64_INT_SIDE_ADMITTED == 0x80'; do
  grep -F "$required" "$root/tests/t-arm64-jit-side-ir-admission.c" >/dev/null || {
    echo "ARM64 side mutation coverage changed: $required" >&2
    exit 1
  }
done
sed -n '/Complete observed n=3\/n=4 return-linked tuple remains closed\./,/expect_semantic(0);/p' \
  "$root/tests/t-arm64-jit-side-ir-admission.c" >"$return_negative_region"
test -s "$return_negative_region"
for required in \
  'fx.view.exitno = 6;' \
  'fx.ir[REF_BASE].op2 = 6;' \
  'set_footer(0, 10, 0);' \
  'set_footer(1, 11, 0);' \
  'fx.ir[K_ADDEND].i = 2;' \
  'fx.view.link = 0;' \
  'fx.view.linktype = LJ_TRLINK_RETURN;' \
  'fx.ir[R_GT].o = IR_LE;' \
  'expect_semantic(0);'; do
  grep -F "$required" "$return_negative_region" >/dev/null || {
    echo "ARM64 side fixture lost combined return-linked negative: $required" >&2
    exit 1
  }
done
test "$(grep -Fc 'set_footer(0, 10, 0);' \
  "$root/tests/t-arm64-jit-side-ir-admission.c")" = 8 || {
  echo "ARM64 side fixture lost exit-6 semantic/pre-head/post-RA coverage" >&2
  exit 1
}
test "$(grep -Fc 'set_footer(1, 11, 0);' \
  "$root/tests/t-arm64-jit-side-ir-admission.c")" = 8 || {
  echo "ARM64 side fixture lost exit-6 second-footer coverage" >&2
  exit 1
}
test "$(grep -Fc 'fx.postra.semantic.exitno = 6;' \
  "$root/tests/t-arm64-jit-side-ir-admission.c")" = 3 || {
  echo "ARM64 side fixture lost exit-6 pre-head/post-RA admission" >&2
  exit 1
}
awk '/^static void make_second_postra\(/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' \
  "$root/tests/t-arm64-jit-side-ir-admission.c" >"$second_postra_region"
test -s "$second_postra_region"
for required in \
  'fx.postra.semantic.exitno = 6;' \
  'fx.ir[REF_BASE].op2 = 6;' \
  'set_footer(0, 10, 0);' \
  'set_footer(1, 11, 0);' \
  'fx.ir[R_PARENT].r = RID_X28;' \
  'fx.ir[R_ADD].r = RID_X27;' \
  'fx.ir[R_LIMIT].r = RID_X28;' \
  'fx.parentmap[0] = REGSP(RID_X27, SPS_NONE);' \
  'A64F_D(RID_X28) | A64F_M(RID_X27))'; do
  grep -F "$required" "$second_postra_region" >/dev/null || {
    echo "ARM64 exit-6 allocator tuple changed: $required" >&2
    exit 1
  }
done
test "$(grep -Fc 'make_second_postra();' \
  "$root/tests/t-arm64-jit-side-ir-admission.c")" = 6 || {
  echo "ARM64 side fixture lost exit-6 allocator/cross-certificate coverage" >&2
  exit 1
}
awk '/^static void make_third_postra\(/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' \
  "$root/tests/t-arm64-jit-side-ir-admission.c" >"$third_postra_region"
test -s "$third_postra_region"
for required in \
  'fx.postra.semantic.exitno = 7;' \
  'fx.ir[REF_BASE].op2 = 7;' \
  'fx.ir[R_PARENT].r = RID_X27;' \
  'fx.ir[R_ADD].r = RID_X28;' \
  'fx.ir[R_LIMIT].r = RID_X27;' \
  'fx.parentmap[0] = REGSP(RID_X28, SPS_NONE);' \
  'A64F_D(RID_X27) | A64F_M(RID_X28))'; do
  grep -F "$required" "$third_postra_region" >/dev/null || {
    echo "ARM64 exit-7 allocator tuple changed: $required" >&2
    exit 1
  }
done
test "$(grep -Fc 'make_third_postra();' \
  "$root/tests/t-arm64-jit-side-ir-admission.c")" = 5 || {
  echo "ARM64 side fixture lost exit-7 allocator/cross-certificate coverage" >&2
  exit 1
}
test "$(grep -Fc 'fx.postra.semantic.exitno = 7;' \
  "$root/tests/t-arm64-jit-side-ir-admission.c")" = 2 || {
  echo "ARM64 side fixture lost exit-7 pre-head/post-RA admission" >&2
  exit 1
}
test "$(grep -Fc 'set_footer(0, 13, 0);' \
  "$root/tests/t-arm64-jit-side-ir-admission.c")" = 2 || {
  echo "ARM64 side fixture lost exit-7/exit-6 geometry cross-certificate" >&2
  exit 1
}
test "$(grep -Fc 'set_footer(1, 14, 0);' \
  "$root/tests/t-arm64-jit-side-ir-admission.c")" = 2 || {
  echo "ARM64 side fixture lost exit-7 second-footer cross-certificate" >&2
  exit 1
}

grep -F 'MSize parentmap_n;  /* Number of entries copied from lj_snap_regspmap(). */' \
  "$asm_source" >/dev/null
grep -F 'as->parentmap_n = parentmap_n;' "$asm_source" >/dev/null

# Build the pure helper directly with function sections so dead-strip can link
# and execute the same fixture under the actual arm64e/BTI compile-time mode.
# This tranche still does not publish an authenticated child target.
# shellcheck disable=SC2086 # arm64e_xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -ffunction-sections -fdata-sections \
  -Wall -Wextra -Werror -arch arm64e -mmacosx-version-min="$minver" \
  $arm64e_xcflags -I"$root/src" \
  -c "$asm_source" -o "$audit_object"
# shellcheck disable=SC2086 # arm64e_xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -ffunction-sections -fdata-sections \
  -Wall -Wextra -Werror -arch arm64e -mmacosx-version-min="$minver" \
  $arm64e_xcflags -I"$root/src" \
  "$root/tests/t-arm64-jit-side-ir-admission.c" "$audit_object" \
  -Wl,-dead_strip -lm -pthread -o "$arm64e_fixture"
"$arm64e_fixture"

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  "$root/tests/t-arm64-jit-side-ir-admission.c" "$archive" -lm -pthread \
  -o "$fixture"
"$fixture"

echo "arm64_jit_side_ir_admission_contract OK: ARM64/arm64e exact first-side semantic, pre-head, post-RA and assembler-consumption certificates verified; broad side gate remains closed"
