#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_ir_admission_contract SKIP: requires native macOS arm64"
  exit 0
fi

if test -z "${SDKROOT:-}"; then
  SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
  export SDKROOT
fi

cc=${CC:-$(xcrun --sdk macosx --find clang)}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
archive=$root/src/libluajit.a
asm_object=$root/src/lj_asm.o
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-ir-admission.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

fixture=$tmpdir/t-arm64-jit-ir-admission
classifier=$tmpdir/classifier.txt
semantic_region=$tmpdir/semantic-region.txt
trace_asm=$tmpdir/trace-asm.txt
call_region=$tmpdir/call-region.txt
value_region=$tmpdir/value-region.txt
postra_region=$tmpdir/postra-region.txt
positive_region=$tmpdir/positive-region.txt
numhalf_region=$tmpdir/numhalf-region.txt
numstep_region=$tmpdir/numstep-region.txt
numacc_region=$tmpdir/numacc-region.txt
audit_object=$tmpdir/lj_asm-arm64e.o
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT'

test -f "$archive" && test -f "$asm_object" || {
  echo "ARM64 IR admission contract requires an existing experimental build" >&2
  exit 1
}

nm "$archive" | grep ' T _lj_asm_arm64_ir_admit$' >/dev/null || {
  echo "experimental archive lacks the ARM64 IR admission gate" >&2
  exit 1
}
nm "$archive" | grep ' T _lj_asm_arm64_postra_admit$' >/dev/null || {
  echo "experimental archive lacks the ARM64 post-RA spill gate" >&2
  exit 1
}

awk '/^\/\* -- Initial ARM64 IR admission/ { copying = 1 }
     copying { print }
     copying && /^\/\* -- Assembler state and common macros/ { exit }' \
  "$root/src/lj_asm.c" >"$classifier"
test -s "$classifier"

awk '/^int lj_asm_arm64_ir_admit/ { copying = 1 }
     copying { print }
     copying && /^\/\* -- Assembler state and common macros/ { exit }' \
  "$root/src/lj_asm.c" >"$semantic_region"
test -s "$semantic_region"

awk '/^static int arm64_ir_int_value_op/ { copying = 1 }
     copying { print }
     copying && /^static int arm64_postra_int_value/ { exit }' \
  "$root/src/lj_asm.c" >"$value_region"
grep -F 'case IR_SLOAD: case IR_ADDOV: case IR_SUBOV: case IR_MULOV:' \
  "$value_region" >/dev/null
grep -F 'case IR_ADD:' "$value_region" >/dev/null
grep -F 'return allow_add;' "$value_region" >/dev/null
grep -F 'static int arm64_ir_num_value_op(IROp op, int allow_sub)' \
  "$value_region" >/dev/null
grep -F 'return op == IR_SLOAD || op == IR_ADD || (allow_sub && op == IR_SUB);' \
  "$value_region" >/dev/null
grep -F 'if (type == IRT_NUM)' "$value_region" >/dev/null
grep -F 'rootop == BC_LOOP && ir.t.irt == (IRT_NUM|IRT_GUARD)' \
  "$value_region" >/dev/null
if grep -E 'case IR_(CONV|SUB|MUL|DIV|LT|GE|LE|GT|EQ|NE|USE|PHI|LOOP|XPOLL):' \
     "$value_region" >/dev/null; then
  echo "non-value IR entered the ARM64 integer producer set" >&2
  exit 1
fi

awk '/^static int arm64_postra_spill_slot/ { copying = 1 }
     copying { print }
     copying && /^static int arm64_ir_int_ref/ { exit }' \
  "$root/src/lj_asm.c" >"$postra_region"
test -s "$postra_region"

awk '/^static void make_trace\(/ { copying = 1 }
     copying { print }
     copying && /^static void make_forl_trace\(/ { exit }' \
  "$root/tests/t-arm64-jit-ir-admission.c" >"$positive_region"
test "$(grep -c 'IR_SLOAD' "$positive_region")" -eq 3
test "$(grep -c 'IR_SUBOV' "$positive_region")" -eq 2
test "$(grep -c 'IR_MULOV' "$positive_region")" -eq 2
test "$(grep -c 'IR_GE' "$positive_region")" -eq 1
test "$(grep -c 'IR_LE' "$positive_region")" -eq 1
test "$(grep -c 'IR_PHI' "$positive_region")" -eq 2
if grep -E 'IR_(KNUM|NUM|CONV|ADD,|SUB,|MUL,|DIV|USE)' \
     "$positive_region" >/dev/null; then
  echo "removed numeric family entered the integer positive fixture" >&2
  exit 1
fi

# Keep the production check ahead of IR scratch growth, compact-trace
# allocation and mcode reservation/publication.
awk '/^void lj_asm_trace\(/ { copying = 1 }
     copying { print }
     copying && /^#if LJ_TARGET_ARM64 && defined\(LJ_ARM64_EMIT_TEST_HELPERS\)/ {
       exit
     }' "$root/src/lj_asm.c" >"$trace_asm"
admit_line=$(grep -n 'lj_asm_arm64_ir_admit(J, T, &reject)' "$trace_asm" | cut -d: -f1)
nextins_line=$(grep -n 'as->orignins = lj_ir_nextins(J)' "$trace_asm" | cut -d: -f1)
reserve_line=$(grep -n 'lj_mcode_reserve(J, &as->mcbot)' "$trace_asm" | cut -d: -f1)
test -n "$admit_line" && test "$admit_line" -lt "$nextins_line" &&
test "$admit_line" -lt "$reserve_line"
grep -F 'lj_trace_err_info(J, LJ_TRERR_NYIIR);' "$trace_asm" >/dev/null
for required in \
  'finalview.nins = arm64_semantic_nins;' \
  'postraview.ir = finalir;' \
  'postraview.proto_bc = proto_bc(J->pt);' \
  'postraview.nins = finalnins;' \
  'postraview.nk = T->nk;' \
  'postraview.spadjust = T->spadjust;' \
  'postraview.proto_sizebc = J->pt->sizebc;' \
  'postraview.root_topslot = T->topslot;' \
  'postraview.startins = T->startins;' \
  'postraview.base_delta = (uint8_t)(J->baseslot-2u);' \
  '!lj_asm_arm64_postra_admit(' \
  'validated_semantic_nins != arm64_semantic_nins' \
  'if (bc_op(T->startins) == BC_FORL)' \
  'T->unused1 |= TRACE_ARM64_INT_FORL_ADMITTED;' \
  'else if (bc_op(T->startins) == BC_FUNCF)' \
  'T->unused1 |= TRACE_ARM64_TRUE_FUNCF_ADMITTED;' \
  'T->unused1 |= TRACE_ARM64_INT_LOOP_ADMITTED;'; do
  grep -F "$required" "$trace_asm" >/dev/null || {
    echo "ARM64 post-RA admission check changed: $required" >&2
    exit 1
  }
done

# The mixed fixture is a second exact grammar, not a widening of the legacy
# integer fixture. Pin its semantic shape, FPR-only post-RA policy, PHI
# consistency, and every adjacent family which remains closed.
for required in \
  'static void make_numeric_trace(jit_State *J)' \
  'assert(numeric_fixture_pt->framesize == 6);' \
  'setir(N_K_ONE, IR_KINT, IRT_INT, 1, 0);' \
  'setir(N_R_X, IR_SLOAD, IRT_NUM|IRT_GUARD,' \
  'setir(N_R_STEP, IR_SLOAD, IRT_NUM|IRT_GUARD,' \
  'setir(N_R_X_PRE, IR_ADD, IRT_NUM|IRT_ISPHI,' \
  'setir(N_R_X_BODY, IR_ADD, IRT_NUM|IRT_ISPHI,' \
  'setir(N_R_X_PHI, IR_PHI, IRT_NUM, N_R_X_PRE, N_R_X_BODY);' \
  'fx.T.nsnap = 7;' \
  'fx.T.nsnapmap = 27;' \
  'setir(N_R_RENAME, IR_RENAME, IRT_NIL, N_R_I_PRE, 4);' \
  'fx.ir[N_R_RENAME].r = RID_X27;' \
  'fx.ir[N_R_X_PRE].r = RID_D15;' \
  'fx.ir[N_R_X_BODY].r = RID_D15;' \
  'fx.ir[N_R_X_PHI].r = RID_D15;' \
  'fx.ir[N_R_X_PHI].r = RID_D14;' \
  'fx.ir[N_R_X_PRE].r = RID_D14;' \
  'fx.ir[N_R_I_PHI].r = RID_X27;' \
  'fx.ir[N_R_X_PHI].s = 2;' \
  'fx.ir[N_R_X].r = RID_X0;' \
  'fx.ir[N_R_X_PRE].s = 2;' \
  'fx.ir[N_R_RENAME].op1 = N_R_X_PRE;' \
  'setir(N_K_ONE, IR_KNUM, IRT_NUM, 0, 0);' \
  'setir(N_R_PRE_GUARD, IR_GT, IRT_NUM|IRT_GUARD,' \
  'REJECT_NUMERIC_ADJACENT(IR_CONV, IRT_NUM|IRT_ISPHI,' \
  'REJECT_NUMERIC_ADJACENT(IR_MUL, IRT_NUM|IRT_ISPHI,' \
  'REJECT_NUMERIC_ADJACENT(IR_DIV, IRT_NUM|IRT_ISPHI,' \
  'setir(N_R_X_PRE, IR_SUB, IRT_NUM|IRT_ISPHI, N_R_STEP, N_R_X);' \
  'expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_SUB);' \
  'setir(N_R_X_PRE, IR_CALLN, IRT_NUM, N_R_X, IRCALL_lj_vm_modi);' \
  'setir(N_R_X_PRE, IR_TNEW, IRT_TAB, 0, 0);' \
  'test_numeric_positive_and_negative(J);' \
  'test_numeric_postra_layout(J);'; do
  grep -F "$required" "$root/tests/t-arm64-jit-ir-admission.c" >/dev/null || {
    echo "ARM64 mixed NUM mutation coverage changed: $required" >&2
    exit 1
  }
done

# The pure-NUM fixture is a third, separate exact grammar. It admits one
# canonical two-slot +0.5 KNUM and ordered FP guards without widening the
# mixed-NUM profile, snapshots, suffix, register file or spill policy.
awk '/^static void make_numhalf_trace\(/ { copying = 1 }
     copying { print }
     copying && /^static void make_numstep_trace\(/ { exit }' \
  "$root/tests/t-arm64-jit-ir-admission.c" >"$numhalf_region"
test -s "$numhalf_region"
test "$(grep -c 'IR_KNUM' "$numhalf_region")" -eq 1
test "$(grep -c 'IR_SLOAD' "$numhalf_region")" -eq 2
test "$(grep -c 'IR_ADD,' "$numhalf_region")" -eq 2
test "$(grep -c 'IR_GT' "$numhalf_region")" -eq 1
test "$(grep -c 'IR_LT' "$numhalf_region")" -eq 1
test "$(grep -c 'IR_LOOP' "$numhalf_region")" -eq 1
test "$(grep -c 'IR_XPOLL' "$numhalf_region")" -eq 1
test "$(grep -c 'IR_PHI' "$numhalf_region")" -eq 1

for required in \
  '#define ARM64_NUMHALF_BITS UINT64_C(0x3fe0000000000000)' \
  'ARM64_NUMHALF_K_HALF = REF_TRUE-2u' \
  'k->o == IR_KNUM && k->t.irt == IRT_NUM && k->op12 == 0' \
  'k[1].tv.u64 == ARM64_NUMHALF_BITS;' \
  'k.r == RID_INIT && k.s == SPS_NONE' \
  'static int arm64_numhalf_snapshots(const SnapShot *snap,' \
  'static const uint16_t mapofs[5] = { 0, 2, 6, 9, 12 };' \
  'static const uint8_t nent[5] = { 0, 2, 1, 1, 1 };' \
  'static const uint8_t nslots[5] = { 4, 5, 4, 4, 4 };' \
  'static const uint8_t pcpos[5] = { 7, 3, 11, 7, 11 };' \
  'nsnap != 5 || nsnapmap != 15 || proto_sizebc != 13' \
  '(uintptr_t)(pcbase >> 8) != expected' \
  'static int arm64_ir_numhalf_bytecode(const GCproto *pt,' \
  'pt->sizebc != 13 || pt->numparams != 1 || pt->sizeuv != 0' \
  'proto_knumtv(pt, 0)->u64 != ARM64_NUMHALF_BITS' \
  'if (startpc != bc+6)' \
  'bc_op(ins) != BC_LOOP || bc_a(ins) != 2 || bc_j(ins) != 4' \
  'bc_op(ins) != BC_ADDVN || bc_a(ins) != 2 ||' \
  'return bc_op(ins) == BC_JMP && bc_a(ins) == 2 && bc_j(ins) == -9;' \
  'static int arm64_ir_numhalf_shape(const jit_State *J, const GCtrace *T,' \
  '!arm64_ir_numhalf_bytecode(pt, trace_startpc_acq((GCtrace *)T))' \
  '!arm64_ir_numhalf_shape(J, T, pt, firstphi, reject)' \
  'static int arm64_postra_numhalf_shape(const LJArm64PostRAView *view,' \
  '!arm64_postra_numhalf_constant(view->ir, view->nk, 1)' \
  '!arm64_postra_numhalf_shape(view, semantic_nins)' \
  'if (!suffix_is_nop || nrename != 0 || spadjust != 0 || highest_end != 0)' \
  'if (constant_profile == ARM64_IR_KPROFILE_HALF)'; do
  grep -F "$required" "$classifier" >/dev/null || {
    echo "ARM64 pure NUM production contract changed: $required" >&2
    exit 1
  }
done

for required in \
  'static void make_numhalf_trace(jit_State *J)' \
  'H_K_HALF = REF_TRUE - 2,' \
  'H_K_HALF_PAYLOAD = REF_TRUE - 1,' \
  'fx.ir[H_K_HALF_PAYLOAD].tv.u64 = UINT64_C(0x3fe0000000000000);' \
  'static const IRRef snaprefs[5] = {' \
  'static const uint16_t mapofs[5] = { 0, 2, 6, 9, 12 };' \
  'static const uint8_t nent[5] = { 0, 2, 1, 1, 1 };' \
  'static const uint8_t nslots[5] = { 4, 5, 4, 4, 4 };' \
  'static const MSize pcpos[5] = { 7, 3, 11, 7, 11 };' \
  'setir(H_R_PRE_GUARD, IR_GT, IRT_NUM|IRT_GUARD,' \
  'setir(H_R_BODY_GUARD, IR_LT, IRT_NUM|IRT_GUARD,' \
  'setir(H_R_X_PHI, IR_PHI, IRT_NUM, H_R_X_PRE, H_R_X_BODY);' \
  'fx.T.nsnap = 5;' \
  'fx.T.nsnapmap = 15;' \
  'fx.ir[H_K_HALF].r = RID_INIT;' \
  'fx.ir[H_R_X].r = RID_D2;' \
  'fx.ir[H_R_X_PRE].r = RID_D15;' \
  'fx.ir[H_R_LIMIT].r = RID_D0;' \
  'setir(H_R_NOP, IR_NOP, IRT_NIL, 0, 0);' \
  'fx.ir[H_K_HALF_PAYLOAD].tv.u64 ^= UINT64_C(1);' \
  'fx.ir[H_K_HALF_PAYLOAD].tv.u64 = UINT64_C(0xbfe0000000000000);' \
  'fx.ir[H_K_HALF_PAYLOAD].tv.u64 = UINT64_C(0x7ff0000000000000);' \
  'fx.ir[H_K_HALF_PAYLOAD].tv.u64 = UINT64_C(0x7ff8000000000000);' \
  'fx.ir[H_K_HALF].r = RID_D0;' \
  'fx.ir[fprrefs[i]].r = RID_X0;' \
  'fx.ir[fprrefs[i]].r = RID_MAX_FPR;' \
  'fx.ir[fprrefs[i]].s = 2;' \
  'setir(H_R_NOP, IR_RENAME, IRT_NIL, H_R_X_PRE, 3);' \
  'IR_LT, IR_GE, IR_LE, IR_EQ, IR_NE' \
  'IR_GT, IR_GE, IR_LE, IR_EQ, IR_NE' \
  'REJECT_NUMHALF_ADJACENT(IR_CONV, IRT_NUM|IRT_ISPHI,' \
  'REJECT_NUMHALF_ADJACENT(IR_SUB, IRT_NUM|IRT_ISPHI,' \
  'REJECT_NUMHALF_ADJACENT(IR_MUL, IRT_NUM|IRT_ISPHI,' \
  'REJECT_NUMHALF_ADJACENT(IR_DIV, IRT_NUM|IRT_ISPHI,' \
  'fx.snapmap[6] = SNAP(3, 0, H_K_HALF);' \
  'fx.snapmap[9] = SNAP(3, SNAP_NORESTORE, H_R_X_PRE);' \
  'proto_bc(numhalf_fixture_pt)+wrong_pcpos[i], 0);' \
  'numhalf_fixture_pt->framesize = 5;' \
  'numhalf_fixture_pt->sizebc = 12;' \
  'test_numhalf_positive_and_negative(J);' \
  'test_numhalf_postra_layout(J);'; do
  grep -F "$required" "$root/tests/t-arm64-jit-ir-admission.c" >/dev/null || {
    echo "ARM64 pure NUM mutation coverage changed: $required" >&2
    exit 1
  }
done

# The dynamic-step pure-NUM fixture is a fourth, separately certified grammar.
# Its prototype uses +0.5 only to initialize x; its IR has no constants and
# carries X, STEP and LIMIT entirely in FPRs.
awk '/^static void make_numstep_trace\(/ { copying = 1 }
     copying { print }
     copying && /^static unsigned numacc_fixture_full_shape\(/ { exit }' \
  "$root/tests/t-arm64-jit-ir-admission.c" >"$numstep_region"
test -s "$numstep_region"
test "$(grep -c 'IR_KNUM' "$numstep_region" || true)" -eq 0
test "$(grep -c 'IR_SLOAD' "$numstep_region")" -eq 3
test "$(grep -c 'IR_ADD,' "$numstep_region")" -eq 2
test "$(grep -c 'IR_GT' "$numstep_region")" -eq 1
test "$(grep -c 'IR_LT' "$numstep_region")" -eq 1
test "$(grep -c 'IR_LOOP' "$numstep_region")" -eq 1
test "$(grep -c 'IR_XPOLL' "$numstep_region")" -eq 1
test "$(grep -c 'IR_PHI' "$numstep_region")" -eq 1

for required in \
  'ARM64_NUMSTEP_R_X = REF_FIRST,' \
  'ARM64_NUMSTEP_R_STEP,' \
  'ARM64_NUMSTEP_SEMANTIC_NINS' \
  'static int arm64_numstep_snapshots(const SnapShot *snap,' \
  'static const uint8_t nslots[5] = { 5, 6, 5, 5, 5 };' \
  'static const uint8_t pcpos[5] = { 7, 3, 12, 7, 12 };' \
  'SNAP(5, 0, ARM64_NUMSTEP_R_X_PRE),' \
  'SNAP(4, 0, ARM64_NUMSTEP_R_X_BODY)' \
  'nsnap != 5 || nsnapmap != 15 || proto_sizebc != 14' \
  'static int arm64_ir_numstep_bytecode(const GCproto *pt,' \
  'pt->sizebc != 14 || pt->numparams != 2 || pt->sizeuv != 0' \
  'pt->sizekn != 1 || pt->sizekgc != 0' \
  'proto_knumtv(pt, 0)->u64 != ARM64_NUMHALF_BITS' \
  'if (startpc != bc+6)' \
  'bc_op(ins) != BC_LOOP || bc_a(ins) != 3 || bc_j(ins) != 5' \
  'bc_op(ins) != BC_ADDVV || bc_a(ins) != 3 ||' \
  'return bc_op(ins) == BC_JMP && bc_a(ins) == 3 && bc_j(ins) == -10;' \
  'static int arm64_ir_numstep_shape(const jit_State *J, const GCtrace *T,' \
  'T->nk != REF_TRUE || T->nins != ARM64_NUMSTEP_SEMANTIC_NINS' \
  '!arm64_ir_numstep_bytecode(pt, trace_startpc_acq((GCtrace *)T))' \
  '!arm64_ir_numstep_shape(J, T, pt, firstphi, reject)' \
  'static int arm64_postra_numstep_shape(const LJArm64PostRAView *view,' \
  'view->nk != REF_TRUE ||' \
  'return arm64_postra_numdynamic_kernel(view, 4, 3, 2,' \
  'ARM64_NUMDYN_ADD_LT);' \
  '} else if (constant_profile == ARM64_IR_KPROFILE_INT) {' \
  '!arm64_postra_numstep_shape(view, semantic_nins)' \
  '} else if (constant_profile == ARM64_IR_KPROFILE_INT &&' \
  'T->nk == REF_TRUE) {' \
  '!arm64_ir_numdynamic_kernel(T, 4, 3, 2, ARM64_NUMDYN_ADD_LT)' \
  'xphi.r == xpre.r && xphi.r == xbody.r &&' \
  'step.r != xphi.r && limit.r != xphi.r && step.r != limit.r &&' \
  'x.r != step.r;'; do
  grep -F "$required" "$classifier" >/dev/null || {
    echo "ARM64 dynamic-step NUM production contract changed: $required" >&2
    exit 1
  }
done

for required in \
  'static void make_numstep_trace(jit_State *J)' \
  'assert(numstep_fixture_pt->framesize == 5);' \
  'assert(numstep_fixture_pt->sizebc == 14);' \
  'assert(numstep_fixture_pt->numparams == 2);' \
  'setir(D_R_X, IR_SLOAD, IRT_NUM|IRT_GUARD,' \
  'setir(D_R_STEP, IR_SLOAD, IRT_NUM|IRT_GUARD,' \
  'setir(D_R_X_PRE, IR_ADD, IRT_NUM|IRT_ISPHI,' \
  'setir(D_R_PRE_GUARD, IR_GT, IRT_NUM|IRT_GUARD,' \
  'setir(D_R_X_BODY, IR_ADD, IRT_NUM|IRT_ISPHI,' \
  'setir(D_R_BODY_GUARD, IR_LT, IRT_NUM|IRT_GUARD,' \
  'setir(D_R_X_PHI, IR_PHI, IRT_NUM, D_R_X_PRE, D_R_X_BODY);' \
  'fx.T.nk = REF_TRUE;' \
  'static const uint8_t nslots[5] = { 5, 6, 5, 5, 5 };' \
  'static const MSize pcpos[5] = { 7, 3, 12, 7, 12 };' \
  'fx.ir[D_R_STEP].r = RID_D1;' \
  'fx.ir[D_R_X_PRE].r = RID_D15;' \
  'fx.ir[D_R_LIMIT].r = RID_D0;' \
  'fx.ir[D_R_X].r = fx.ir[D_R_LIMIT].r;' \
  'fx.ir[D_R_X].r = fx.ir[D_R_X_PHI].r;' \
  'fx.ir[D_R_STEP].r = fx.ir[D_R_X_PHI].r;' \
  'fx.ir[D_R_LIMIT].r = fx.ir[D_R_X_PHI].r;' \
  'fx.ir[D_R_STEP].r = fx.ir[D_R_LIMIT].r;' \
  'fx.ir[D_R_X].r = fx.ir[D_R_STEP].r;' \
  'fx.ir[value_refs[i]].r = RID_X0;' \
  'fx.ir[value_refs[i]].r = RID_MAX_FPR;' \
  'fx.ir[value_refs[i]].s = 2;' \
  'setir(D_R_NOP, IR_RENAME, IRT_NIL, D_R_X_PRE, 3);' \
  'fx.T.nk = REF_TRUE-1u;' \
  'fx.T.nk = H_K_HALF;' \
  'REJECT_NUMSTEP_ADJACENT(IR_CONV, IRT_NUM|IRT_ISPHI,' \
  'REJECT_NUMSTEP_ADJACENT(IR_SUB, IRT_NUM|IRT_ISPHI,' \
  'REJECT_NUMSTEP_ADJACENT(IR_MUL, IRT_NUM|IRT_ISPHI,' \
  'REJECT_NUMSTEP_ADJACENT(IR_DIV, IRT_NUM|IRT_ISPHI,' \
  'fx.snapmap[9] = SNAP(4, SNAP_NORESTORE, D_R_X_PRE);' \
  'bc_publish((const uint32_t *)pc, saved ^ masks[bitno]);' \
  'numstep_fixture_pt->framesize = 4;' \
  'numstep_fixture_pt->sizebc = 13;' \
  'numstep_fixture_pt->numparams = 1;' \
  'numstep_fixture_pt->sizeuv = 1;' \
  'numstep_fixture_pt->sizekn = 0;' \
  'numstep_fixture_pt->sizekgc = 1;' \
  'proto_knumtv(numstep_fixture_pt, 0)->u64 = UINT64_C(0xbfe0000000000000);' \
  'proto_knumtv(numstep_fixture_pt, 0)->u64 = UINT64_C(0x7ff0000000000000);' \
  'proto_knumtv(numstep_fixture_pt, 0)->u64 = UINT64_C(0x7ff8000000000000);' \
  'test_numstep_positive_and_negative(J);' \
  'test_numstep_postra_layout(J);'; do
  grep -F "$required" "$root/tests/t-arm64-jit-ir-admission.c" >/dev/null || {
    echo "ARM64 dynamic-step NUM mutation coverage changed: $required" >&2
    exit 1
  }
done

# The dynamic-accumulator pure-NUM fixture is one exact geometry with ADD_LT,
# ADD_LE and SUB_GT full-shape profiles. All three scalars are parameters, so
# both trace and prototype constant sets are empty; exact comparison operands
# and recurrence bytecode select the only matching IR tuple.
awk '/^static unsigned numacc_fixture_full_shape\(/ { copying = 1 }
     copying { print }
     copying && /^static LJArm64IRReject expect_reject\(/ { exit }' \
  "$root/tests/t-arm64-jit-ir-admission.c" >"$numacc_region"
test -s "$numacc_region"
test "$(grep -c 'IR_KNUM' "$numacc_region" || true)" -eq 0
test "$(grep -c 'IR_KINT' "$numacc_region" || true)" -eq 0
test "$(grep -c 'IR_SLOAD' "$numacc_region")" -eq 3
test "$(grep -c 'profile->recurrence_op' "$numacc_region")" -eq 2
test "$(grep -c 'profile->precondition_op' "$numacc_region")" -eq 1
test "$(grep -c 'profile->body_op' "$numacc_region")" -eq 1
test "$(grep -c 'IR_LOOP' "$numacc_region")" -eq 1
test "$(grep -c 'IR_XPOLL' "$numacc_region")" -eq 1
test "$(grep -c 'IR_PHI' "$numacc_region")" -eq 1

for required in \
  'ARM64_NUMDYN_ADD_LT = 1u,' \
  'ARM64_NUMDYN_ADD_LE = 2u,' \
  'ARM64_NUMDYN_SUB_GT = 3u' \
  'ARM64_NUMACC_R_X = ARM64_NUMSTEP_R_X,' \
  'ARM64_NUMACC_R_STEP = ARM64_NUMSTEP_R_STEP,' \
  'ARM64_NUMACC_SEMANTIC_NINS = ARM64_NUMSTEP_SEMANTIC_NINS' \
  'static int arm64_numacc_snapshots(const SnapShot *snap,' \
  'static const uint8_t nslots[5] = { 5, 6, 5, 5, 5 };' \
  'static const uint8_t pcpos[5] = { 6, 2, 11, 6, 11 };' \
  'SNAP(2, 0, ARM64_NUMACC_R_X_PRE),' \
  'SNAP(5, 0, ARM64_NUMACC_R_X_PRE),' \
  'SNAP(2, 0, ARM64_NUMACC_R_X_BODY)' \
  'nsnap != 5 || nsnapmap != 15 || proto_sizebc != 13' \
  'static int arm64_postra_numdynamic_kernel(const LJArm64PostRAView *view,' \
  'IRRef xslot, IRRef stepslot, IRRef limitslot,' \
  'unsigned grammar_profile)' \
  'if (grammar_profile == ARM64_NUMDYN_ADD_LT) {' \
  '} else if (grammar_profile == ARM64_NUMDYN_ADD_LE) {' \
  '} else if (grammar_profile == ARM64_NUMDYN_SUB_GT) {' \
  'recurrence_op = IR_SUB;' \
  'first_left = ARM64_NUMSTEP_R_X;' \
  'first_right = ARM64_NUMSTEP_R_STEP;' \
  'preop = IR_GE;' \
  'bodyop = IR_LE;' \
  'preop = IR_LT;' \
  'bodyop = IR_GT;' \
  'ARM64_NUMSTEP_POSTRA_INS(ARM64_NUMSTEP_R_X_PRE, recurrence_op,' \
  'IRT_NUM|IRT_ISPHI, first_left, first_right)' \
  'ARM64_NUMSTEP_POSTRA_INS(ARM64_NUMSTEP_R_X_BODY, recurrence_op,' \
  'return arm64_postra_numdynamic_kernel(view, 2, 4, 3,' \
  'static int arm64_postra_numacc_shape(const LJArm64PostRAView *view,' \
  'static unsigned arm64_numacc_grammar_profile(const BCIns *proto_bc,' \
  'bc_op(recurrence) == BC_ADDVV && bc_a(recurrence) == 3' \
  'return ARM64_NUMDYN_ADD_LT;' \
  'return ARM64_NUMDYN_ADD_LE;' \
  'bc_op(recurrence) == BC_SUBVV && bc_a(recurrence) == 3' \
  'bc_op(compare) == BC_ISGE && bc_a(compare) == 4' \
  'return ARM64_NUMDYN_SUB_GT;' \
  'unsigned grammar_profile = arm64_numacc_grammar_profile(' \
  'view->root_topslot != 5 || view->proto_sizebc != 13 ||' \
  'grammar_profile == 0 ||' \
  'semantic_nins != ARM64_NUMACC_SEMANTIC_NINS ||' \
  '!arm64_numacc_snapshots(view->snap, view->snapmap,' \
  'else if (view->proto_sizebc == 13) {' \
  '!arm64_postra_numacc_shape(view, semantic_nins)' \
  'static int arm64_ir_numacc_bytecode(const GCproto *pt,' \
  'const BCIns *startpc, unsigned *grammar_profile)' \
  'startpc == NULL || grammar_profile == NULL ||' \
  'pt->sizebc != 13 || pt->numparams != 3 || pt->sizeuv != 0 ||' \
  'pt->sizekn != 0 || pt->sizekgc != 0 ||' \
  'pt->flags2 != PROTO2_CELLOPS' \
  'if (startpc != bc+5)' \
  'ARM64_NUMACC_BC_AD(0, BC_FUNCF, 5, 0)' \
  'ARM64_NUMACC_BC_AD(1, BC_CGET, 3, 0)' \
  'ARM64_NUMACC_BC_AD(2, BC_CGET, 4, 1)' \
  'ARM64_NUMACC_BC_AD(6, BC_CGET, 3, 0)' \
  'ARM64_NUMACC_BC_AD(7, BC_CGET, 4, 2)' \
  'ARM64_NUMACC_BC_AD(9, BC_CSET, 0, 3)' \
  'ARM64_NUMACC_BC_AD(11, BC_CGET, 3, 0)' \
  'ARM64_NUMACC_BC_AD(12, BC_RET1, 3, 2)' \
  'bc_op(ins) != BC_LOOP || bc_a(ins) != 3 || bc_j(ins) != 5' \
  'arm64_numacc_grammar_profile(bc, pt->sizebc) != profile)' \
  '*grammar_profile = profile;' \
  'static int arm64_ir_numdynamic_kernel(const GCtrace *T, IRRef xslot,' \
  'IRRef stepslot, IRRef limitslot, unsigned grammar_profile)' \
  'ARM64_NUMSTEP_INS(ARM64_NUMSTEP_R_X_PRE, recurrence_op,' \
  'ARM64_NUMSTEP_INS(ARM64_NUMSTEP_R_X_BODY, recurrence_op,' \
  'bc_b(ins) != 3 || bc_c(ins) != 4)' \
  'static int arm64_ir_numacc_shape(const jit_State *J, const GCtrace *T,' \
  'T->nk != REF_TRUE || T->nins != ARM64_NUMACC_SEMANTIC_NINS' \
  '!arm64_ir_numacc_bytecode(pt, trace_startpc_acq((GCtrace *)T),' \
  '!arm64_numacc_snapshots(T->snap, T->snapmap, T->nsnap,' \
  '!arm64_ir_numdynamic_kernel(T, 2, 4, 3, grammar_profile)' \
  'allow_num_sub = numdynamic_profile == ARM64_NUMDYN_SUB_GT;' \
  'if (!allow_num_sub || startop != BC_LOOP ||' \
  'if (!allow_num_sub || irt_type(ins.t) != IRT_NUM)' \
  '!arm64_ir_num_ref(T, ref, snapref, allow_num_sub)' \
  'else if (pt->sizebc == 13) {' \
  '!arm64_ir_numacc_shape(J, T, pt, firstphi, reject)'; do
  grep -F "$required" "$classifier" >/dev/null || {
    echo "ARM64 dynamic-accumulator NUM production contract changed: $required" >&2
    exit 1
  }
done

for required in \
  'NUMACC_FIXTURE_ADD_LT = 1u,' \
  'NUMACC_FIXTURE_ADD_LE = 2u,' \
  'NUMACC_FIXTURE_SUB_GT = 3u' \
  'typedef struct NumaccFixtureProfile {' \
  'BCOp comparison_bc;' \
  'BCOp recurrence_bc;' \
  'IROp recurrence_op;' \
  '{ NUMACC_FIXTURE_ADD_LT, BC_ISGE, 3, 4, BC_ADDVV, IR_ADD,' \
  '{ NUMACC_FIXTURE_ADD_LE, BC_ISGT, 3, 4, BC_ADDVV, IR_ADD,' \
  '{ NUMACC_FIXTURE_SUB_GT, BC_ISGE, 4, 3, BC_SUBVV, IR_SUB,' \
  'static void select_numacc_fixture(unsigned profile_id)' \
  'static unsigned numacc_fixture_full_shape(void)' \
  'bc_op(arithmetic) == profile->recurrence_bc' \
  'static const NumaccFixtureProfile *numacc_active_profile(void)' \
  'numacc_fixture_full_shape() == numacc_fixture_profile->id' \
  'static void make_numacc_trace(jit_State *J)' \
  'assert(numacc_fixture_pt->framesize == 5);' \
  'assert(numacc_fixture_pt->sizebc == 13);' \
  'assert(numacc_fixture_pt->numparams == 3);' \
  'setir(A_R_X, IR_SLOAD, IRT_NUM|IRT_GUARD,' \
  'setir(A_R_STEP, IR_SLOAD, IRT_NUM|IRT_GUARD,' \
  'setir(A_R_X_PRE, profile->recurrence_op, IRT_NUM|IRT_ISPHI,' \
  'setir(A_R_LIMIT, IR_SLOAD, IRT_NUM|IRT_GUARD,' \
  'setir(A_R_PRE_GUARD, profile->precondition_op, IRT_NUM|IRT_GUARD,' \
  'setir(A_R_X_BODY, profile->recurrence_op, IRT_NUM|IRT_ISPHI,' \
  'setir(A_R_BODY_GUARD, profile->body_op, IRT_NUM|IRT_GUARD,' \
  'setir(A_R_X_PHI, IR_PHI, IRT_NUM, A_R_X_PRE, A_R_X_BODY);' \
  'fx.snapmap[2] = SNAP(2, 0, A_R_X_PRE);' \
  'fx.snapmap[3] = SNAP(5, 0, A_R_X_PRE);' \
  'fx.snapmap[12] = SNAP(2, 0, A_R_X_BODY);' \
  'static const MSize pcpos[5] = { 6, 2, 11, 6, 11 };' \
  'fx.T.nk = REF_TRUE;' \
  'fx.ir[A_R_STEP].r = RID_D1;' \
  'fx.ir[A_R_X_PRE].r = RID_D15;' \
  'fx.ir[A_R_LIMIT].r = RID_D0;' \
  'fx.ir[A_R_X].r = fx.ir[A_R_LIMIT].r;' \
  'fx.ir[A_R_X].r = fx.ir[A_R_X_PHI].r;' \
  'fx.ir[A_R_STEP].r = fx.ir[A_R_X_PHI].r;' \
  'fx.ir[A_R_LIMIT].r = fx.ir[A_R_X_PHI].r;' \
  'fx.ir[A_R_STEP].r = fx.ir[A_R_LIMIT].r;' \
  'fx.ir[A_R_X].r = fx.ir[A_R_STEP].r;' \
  'fx.ir[value_refs[i]].r = RID_X0;' \
  'fx.ir[value_refs[i]].r = RID_MAX_FPR;' \
  'fx.ir[value_refs[i]].s = 2;' \
  'setir(A_R_NOP, IR_RENAME, IRT_NIL, A_R_X_PRE, 3);' \
  'fx.ir[A_R_X].op1 = 4;' \
  'fx.ir[D_R_X].op1 = 2;' \
  'IR_ULT, IR_UGE, IR_ULE, IR_UGT' \
  'fx.ir[A_R_PRE_GUARD].op1 = A_R_X_PRE;' \
  'fx.ir[A_R_PRE_GUARD].op2 = A_R_LIMIT;' \
  'fx.ir[A_R_BODY_GUARD].op1 = A_R_LIMIT;' \
  'fx.ir[A_R_BODY_GUARD].op2 = A_R_X_BODY;' \
  'fx.T.nk = REF_TRUE-1u;' \
  'fx.T.nk = H_K_HALF;' \
  'REJECT_NUMACC_ADJACENT(IR_CONV, IRT_NUM|IRT_ISPHI,' \
  'REJECT_NUMACC_ADJACENT(IR_SUB, IRT_NUM|IRT_ISPHI,' \
  'REJECT_NUMACC_ADJACENT(IR_MUL, IRT_NUM|IRT_ISPHI,' \
  'REJECT_NUMACC_ADJACENT(IR_DIV, IRT_NUM|IRT_ISPHI,' \
  'fx.snapmap[9] = SNAP(2, SNAP_NORESTORE, A_R_X_PRE);' \
  'for (i = 0; i < 13; i++) {' \
  'bc_publish((const uint32_t *)pc, saved ^ masks[bitno]);' \
  'BCINS_AD(bc_op(saved), profile->comparison_d,' \
  'BCINS_ABC(adjacent, 3, 3, 4));' \
  'numacc_fixture_pt->framesize = 4;' \
  'numacc_fixture_pt->framesize = 6;' \
  'numacc_fixture_pt->sizebc = 12;' \
  'numacc_fixture_pt->sizebc = 14;' \
  'numacc_fixture_pt->numparams = 2;' \
  'numacc_fixture_pt->numparams = 4;' \
  'numacc_fixture_pt->sizeuv = 1;' \
  'numacc_fixture_pt->sizekn = 1;' \
  'numacc_fixture_pt->sizekgc = 1;' \
  'numacc_fixture_pt->flags2 = 0;' \
  'numacc_fixture_pt->flags2 = PROTO2_CELLOPS|PROTO2_CELLUV;' \
  'assert(numacc_fixture_loop_pc == proto_bc(numacc_fixture_pt)+5);' \
  'while x<=limit do x=x+step end return x end' \
  'while x>limit do x=x-step end return x end' \
  'numacc_strict_fixture_pt = funcproto(funcV(L->top-1));' \
  'numacc_inclusive_fixture_pt = funcproto(funcV(L->top-1));' \
  'numacc_sub_gt_fixture_pt = funcproto(funcV(L->top-1));' \
  'bc_op(loadbc(proto_bc(numacc_fixture_pt)+3)) == BC_ISGE' \
  'bc_op(loadbc(proto_bc(numacc_inclusive_fixture_pt)+3)) == BC_ISGT' \
  'bc_a(comparison) == 4 && bc_d(comparison) == 3' \
  'bc_op(arithmetic) == BC_SUBVV && bc_a(arithmetic) == 3' \
  'static void test_numacc_shape_cross_product(jit_State *J)' \
  'static const IROp arithmetic_ops[2] = { IR_ADD, IR_SUB };' \
  'static const IROp preops[3] = { IR_GT, IR_GE, IR_LT };' \
  'static const IROp bodyops[3] = { IR_LT, IR_LE, IR_GT };' \
  'int admitted = pre_arithmetic == expected_arithmetic &&' \
  'assert(combinations == 3u*2u*2u*3u*3u);' \
  'assert(combinations == 108);' \
  'assert(semantic_admissions == 3 && postra_admissions == 3);' \
  'expect_numacc_semantic_result(J, admitted);' \
  'expect_numacc_postra_result(&view, admitted);' \
  'BCINS_AD(BC_ISGT, 4, 3)' \
  'fx.ir[A_R_PRE_GUARD].o = IR_LE;' \
  'fx.ir[A_R_BODY_GUARD].o = IR_GE;' \
  'select_numacc_fixture(NUMACC_FIXTURE_ADD_LT);' \
  'select_numacc_fixture(NUMACC_FIXTURE_ADD_LE);' \
  'select_numacc_fixture(NUMACC_FIXTURE_SUB_GT);' \
  'test_numacc_shape_cross_product(J);' \
  'L->top -= 7;' \
  'test_numacc_positive_and_negative(J);' \
  'test_numacc_postra_layout(J);'; do
  grep -F "$required" "$root/tests/t-arm64-jit-ir-admission.c" >/dev/null || {
    echo "ARM64 dynamic-accumulator NUM mutation coverage changed: $required" >&2
    exit 1
  }
done
test "$(grep -Fc 'test_numacc_positive_and_negative(J);' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 3
test "$(grep -Fc 'test_numacc_postra_layout(J);' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 3
postra_line=$(grep -n '!lj_asm_arm64_postra_admit(' "$trace_asm" | cut -d: -f1)
marker_line=$(grep -n 'T->unused1 |= TRACE_ARM64_INT_FORL_ADMITTED;' \
  "$trace_asm" | cut -d: -f1)
test -n "$postra_line" && test -n "$marker_line" &&
test "$postra_line" -lt "$marker_line"
if grep -F 'T->spadjust != 0 || as->evenspill' "$trace_asm" >/dev/null; then
  echo "ARM64 assembly gate still keys spill admission to allocator cursors" >&2
  exit 1
fi

for required in \
  'LJ_STATIC_ASSERT(SPS_FIRST == 2);' \
  'LJ_STATIC_ASSERT(SPS_FIXED == 4);' \
  'LJ_STATIC_ASSERT(SPS_LIMIT == 256);' \
  'view->nk == 0 || view->nk > REF_TRUE' \
  'view->root_topslot > UINT8_MAX || view->base_delta != 0' \
  '(UINTPTR_MAX-proto_lo)/sizeof(BCIns)' \
  'spadjust > (MSize)sps_scale(SPS_LIMIT-SPS_FIXED)' \
  'capacity = SPS_FIXED + spadjust / sizeof(int32_t);' \
  'slot >= SPS_FIRST && slot < capacity && slot < SPS_LIMIT' \
  'if (last.o == IR_NOP)' \
  'nrename == 0 || nrename > LJ_MAX_PHI' \
  'case IR_SLOAD:' \
  'case IR_ADDOV: case IR_SUBOV: case IR_MULOV:' \
  'case IR_ADD:' \
  'case IR_LT: case IR_GE: case IR_LE: case IR_GT:' \
  'case IR_EQ: case IR_NE:' \
  'case IR_LOOP: case IR_XPOLL:' \
  'case IR_PHI:' \
  'MSize expected = (MSize)sps_scale(sps_align(highest_end));' \
  'spadjust != expected || highest_end > capacity' \
  'ren.op2 >= nsnap || ren.s != SPS_NONE' \
  'if (irt_type(source.t) == IRT_INT)' \
  'ren.r >= RID_MAX_GPR || !rset_test(RSET_GPR, ren.r)' \
  'else if (irt_type(source.t) == IRT_NUM)' \
  'ren.r < RID_MIN_FPR || ren.r >= RID_MAX_FPR ||' \
  '!rset_test(RSET_FPR, ren.r)' \
  'if (!arm64_postra_int_value(source, rootop, forl_idxslot, maxslots) ||' \
  'slot != SPS_NONE || ins.r < RID_MIN_FPR || ins.r >= RID_MAX_FPR ||' \
  'constant_profile != ARM64_IR_KPROFILE_INT || suffix_is_nop ||' \
  'nrename != 1u || spadjust != 0 || highest_end != 0' \
  'return iphi.r == ipre.r && iphi.r == ibody.r &&' \
  'xphi.r == xpre.r && xphi.r == xbody.r;' \
  'mapofs != expected_mapofs || nent > nextofs-mapofs' \
  'nextofs-mapofs-nent != 1u+LJ_FR2' \
  'snapat < REF_FIRST || snapat >= semantic_nins' \
  'snapat < prev_snapref' \
  'topslot != view->root_topslot' \
  'nslots > view->root_topslot+1u+LJ_FR2' \
  'slot >= nslots || (n != 0 &&' \
  'sn == SNAP(1, SNAP_FRAME|SNAP_NORESTORE, REF_NIL)' \
  '(flags != 0 && flags != SNAP_NORESTORE)' \
  'flags != 0 || valueref < view->nk || valueref >= REF_TRUE' \
  'valueref < REF_FIRST || valueref >= snapat' \
  'slot != forl_idxslot && slot != forl_idxslot+FORL_STOP' \
  'rs = source.prev;' \
  'for (renref = view->nins; renref-- > semantic_nins; )' \
  'if (ren.op1 == valueref && ren.op2 <= snapno)' \
  'if (ra_hasspill(regsp_spill(rs)))' \
  'if (irt_type(source.t) == IRT_NUM ||' \
  'regsp_reg(rs) < RID_MIN_FPR || regsp_reg(rs) >= RID_MAX_FPR ||' \
  'regsp_reg(rs) >= RID_MAX_GPR ||' \
  '!rset_test(RSET_GPR, regsp_reg(rs))' \
  'pcraw[n] = snapentry_acq(&view->snapmap[mapofs+nent+n]);' \
  '(uint8_t)pcbase != view->base_delta' \
  '!arm64_ir_pcpos(snappc, proto_lo, proto_hi, &snappos)'; do
  grep -F "$required" "$postra_region" >/dev/null || {
    echo "ARM64 post-RA spill invariant changed: $required" >&2
    exit 1
  }
done
rename_range_line=$(grep -n 'ren.op2 >= nsnap || ren.s != SPS_NONE' \
  "$postra_region" | cut -d: -f1)
rename_rset_line=$(grep -n 'ren.r >= RID_MAX_GPR || !rset_test(RSET_GPR, ren.r)' \
  "$postra_region" | cut -d: -f1)
rename_fpr_line=$(grep -n 'ren.r < RID_MIN_FPR || ren.r >= RID_MAX_FPR ||' \
  "$postra_region" | cut -d: -f1)
snap_range_line=$(grep -n 'regsp_reg(rs) >= RID_MAX_GPR ||' \
  "$postra_region" | cut -d: -f1)
snap_rset_line=$(grep -n '!rset_test(RSET_GPR, regsp_reg(rs))' \
  "$postra_region" | cut -d: -f1)
test -n "$rename_range_line" && test -n "$rename_rset_line" &&
test -n "$rename_fpr_line" && test "$rename_range_line" -lt "$rename_rset_line" &&
test "$rename_range_line" -lt "$rename_fpr_line"
test -n "$snap_range_line" && test -n "$snap_rset_line" &&
test "$snap_range_line" -lt "$snap_rset_line"
if grep -E 'lj_mcode_|trace_save|traceslot_publish|lj_ir_call|asm_call|lj_trace_err' \
     "$postra_region" >/dev/null; then
  echo "ARM64 post-RA spill validator gained side effects" >&2
  exit 1
fi

if grep -E 'lj_mcode_|trace_save|traceslot_publish|lj_ir_call|asm_call' \
     "$classifier" >/dev/null; then
  echo "ARM64 IR classifier acquired code/publication/helper side effects" >&2
  exit 1
fi

# Exact candidate opcode inventory. CALL opcodes appear only so they can report
# a helper ID and reject it; the admitted helper-ID set remains empty.
for cases in \
  'case IR_BASE:' \
  'case IR_SLOAD:' \
  'case IR_LT: case IR_GE: case IR_LE: case IR_GT:' \
  'case IR_EQ: case IR_NE:' \
  'case IR_ADDOV: case IR_SUBOV: case IR_MULOV:' \
  'case IR_ADD:' \
  'case IR_SUB:' \
  'case IR_PHI:' \
  'case IR_LOOP:' \
  'case IR_XPOLL:' \
  'case IR_CALLN: case IR_CALLA: case IR_CALLL: case IR_CALLS:' \
  'case IR_CALLXS:'; do
  grep -F "$cases" "$semantic_region" >/dev/null || {
    echo "ARM64 IR classifier inventory changed: $cases" >&2
    exit 1
  }
done

awk '/case IR_CALLN:/ { copying = 1 }
     copying { print }
     copying && /default:/ { exit }' "$semantic_region" >"$call_region"
test "$(grep -c 'LJ_ARM64_IR_REJECT_CALL' "$call_region")" -eq 2
if grep -E 'break;|return 1|IRCALL_[A-Za-z0-9_]+[[:space:]]*:' \
     "$call_region" >/dev/null; then
  echo "ARM64 CALL helper allowlist is no longer empty" >&2
  exit 1
fi

for forbidden in IR_KGC IR_KPTR IR_KKPTR IR_KNULL IR_KINT64 IR_KSLOT \
  IR_NOP IR_CONV IR_MUL IR_DIV IR_ULT IR_UGE IR_ULE IR_UGT \
  IR_USE IR_NEG IR_MOD IR_POW \
  IR_ABS IR_LDEXP \
  IR_MIN IR_MAX IR_FPMATH \
  IR_AREF IR_HREF IR_UREFO IR_FLOAD IR_XLOAD IR_ASTORE IR_HSTORE \
  IR_USTORE IR_FSTORE IR_XSTORE IR_SNEW IR_TNEW IR_CNEW IR_BUFHDR \
  IR_TBAR IR_OBAR IR_XBAR IR_XSAVE IR_RETF IR_PROF IR_CARG; do
  if grep -E "case[[:space:]]+$forbidden:" "$semantic_region" >/dev/null; then
    echo "forbidden ARM64 IR unexpectedly gained an admitted case: $forbidden" >&2
    exit 1
  fi
done

for required in \
  'T->sinktags != 0' \
  'for (ref = REF_TRUE; ref <= REF_NIL; ref++)' \
  'ir->o != IR_KPRI || ir->t.irt != expected || ir->op12 != 0' \
  'ir->o != IR_KINT || ir->t.irt != IRT_INT' \
  'arm64_ir_int_value_op((IROp)ir->o, allow_add)' \
  'arm64_ir_num_value_op((IROp)ir->o, allow_sub)' \
  'startop != BC_LOOP || ir->t.irt != (IRT_NUM|IRT_ISPHI)' \
  '!arm64_ir_num_binary(T, ir, ref, allow_num_sub)' \
  'scalar_mode == (ARM64_IR_SCALAR_INT|ARM64_IR_SCALAR_NUM)' \
  'scalar_mode == ARM64_IR_SCALAR_NUM' \
  '!arm64_ir_numadd_shape(T, firstphi, reject)' \
  'arm64_ir_proto_range(pt, &lo, &hi)' \
  'startpc == NULL ||' \
  'startpc != J->startpc' \
  'arm64_ir_bc_acq(lo, pos) != startins' \
  'bc_op(back) != BC_JMP' \
  'target > (int64_t)pos' \
  '(MSize)slot > (MSize)pt->framesize' \
  'ir->op1 < 1 + LJ_FR2 || ir->op1 >= maxslots ||' \
  'ir->op1 >= root_topslot + 1u + LJ_FR2' \
  'ir.op2 == IRSLOAD_TYPECHECK' \
  'ref >= before' \
  'mapofs > T->nsnapmap || nextofs > T->nsnapmap' \
  'mapofs != expected_mapofs' \
  'nextofs - mapofs - nent != 1u + LJ_FR2' \
  'slot >= nslots' \
  'slot <= snap_slot(T->snapmap[mapofs+n-1])' \
  'uint32_t flags = sn & 0x00ff0000u;' \
  '(flags != 0 && flags != SNAP_NORESTORE)' \
  '(uint8_t)pcbase != (uint8_t)(J->baseslot-2)' \
  '!arm64_ir_pcpos(snappc, proto_lo, proto_hi, &snappos)' \
  'topslot != root_topslot' \
  'nslots > root_topslot + 1u + LJ_FR2' \
  'T->snap[0].ref > ref' \
  'xpollref != loopref + 1u' \
  'xpollsnap != loopref' \
  'ir->op1 != 1' \
  'J->loopref != loopref' \
  'firstphi != 0 && ir->o != IR_PHI' \
  'ir->op1 < REF_FIRST || ir->op1 >= loopref' \
  'ir->op2 <= xpollref || ir->op2 >= firstphi' \
  'nphi > LJ_MAX_PHI' \
  '!irt_isphi(T->ir[ir->op1].t)' \
  '!irt_isphi(T->ir[ir->op2].t)' \
  'T->ir[prevphi].op1 == ir->op1' \
  '!!irt_isphi(T->ir[ref].t) != operand' \
  '(startop != BC_LOOP && startop != BC_FORL)'; do
  grep -F "$required" "$classifier" >/dev/null || {
    echo "ARM64 IR structural check changed: $required" >&2
    exit 1
  }
done

# The synthetic admission fixture exercises real prototype bytecode and each
# structural corruption which must remain fail-closed before publication.
for required in \
  'fixture_snapshot_pc = fixture_forl_pc + 1 + bc_j(loadbc(fixture_forl_pc));' \
  'set_snapshot_payload(2, fixture_snapshot_pc, 1);' \
  'set_snapshot_payload(2, (const BCIns *)(lo+1), 0);' \
  'set_snapshot_payload(2, (const BCIns *)(lo-sizeof(BCIns)), 0);' \
  'set_snapshot_payload(2, (const BCIns *)hi, 0);' \
  'setmref(fx.T.startpc, fixture_snapshot_pc);' \
  'fx.T.startins = BCINS_AJ(BC_LOOP, bc_a(loop)^1u, bc_j(loop));' \
  'fixture_pt->sizebc = 0;' \
  'fixture_pt->sizebc = (MSize)(fixture_loop_pc-bc);' \
  'make_forl_trace(J);' \
  'fx.ir[R_B].op2 = IRSLOAD_TYPECHECK|IRSLOAD_INHERIT;' \
  'fx.ir[R_C].op2 = IRSLOAD_READONLY|IRSLOAD_INHERIT;' \
  'expect_reject(J, LJ_ARM64_IR_REJECT_OPERAND, IR_ADD);' \
  'fx.T.startins = loadbc(fixture_forl_pc);' \
  'setir(REF_TRUE, IR_KINT, IRT_INT, 1, 0);' \
  'setir(REF_FALSE, IR_KNUM, IRT_NUM, 0, 0);' \
  'setir(K_STOP, IR_KNUM, IRT_NUM, 0, 0);' \
  'fx.ir[H_K_HALF_PAYLOAD].tv.u64 = UINT64_C(0x3fe0000000000000);' \
  'fx.ir[R_PRECOND].op2 = R_A;' \
  'fx.ir[R_BODY2].op2 = R_C;' \
  'fx.ir[R_LOOPCOND].op2 = R_A;' \
  'fixture_loop_pc-loopbackpc));' \
  'BCINS_AJ(BC_LOOP, fixture_pt->framesize+1u, bc_j(loop))' \
  'fx.ir[R_PHI2].op1 = R_SUM1;' \
  'LJ_STATIC_ASSERT(REF_FIRST + 3*LJ_MAX_PHI + 2u <= ADMISSION_IR_CAP);' \
  'sizeof(fx.ir)/sizeof(fx.ir[0])' \
  'phi = post + LJ_MAX_PHI + 1u;' \
  'fx.T.nins = end;' \
  'fx.snap[3].ref = R_XPOLL;' \
  'fx.snapmap[6] = SNAP(2, SNAP_NORESTORE, R_SUM1);' \
  'fx.ir[R_BODY1].op1 = R_PRECOND;' \
  'REJECT_REMOVED(R_SUM1, IR_USE, IRT_INT, R_A, 0);' \
  'setir(R_SUM1, IR_SUB, IRT_INT, R_A, R_B);' \
  'expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_SUB);' \
  'expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_ADD);' \
  'IR_LT, IR_GE, IR_LE, IR_GT, IR_EQ, IR_NE' \
  'IR_ADDOV, IR_SUBOV, IR_MULOV' \
  'IR_ULT, IR_UGE, IR_ULE, IR_UGT' \
  'expect_reject(J, LJ_ARM64_IR_REJECT_OPCODE, unsigned_guards[guardno]);' \
  'setir(R_SUM1, IR_ADDOV, IRT_INT|IRT_ISPHI, R_A, R_B);' \
  'fx.ir[R_SUM2].t.irt &= (uint8_t)~IRT_GUARD;' \
  'expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_MULOV);' \
  'fx.snap[0].mapofs = fx.T.nsnapmap+1u;' \
  'fx.snap[1].mapofs = fx.T.nsnapmap+1u;' \
  'fx.ir[R_A].t.irt = IRT_NUM|IRT_GUARD;'; do
  grep -F "$required" "$root/tests/t-arm64-jit-ir-admission.c" >/dev/null || {
    echo "ARM64 IR negative fixture coverage changed: $required" >&2
    exit 1
  }
done

for required in \
  'fx.ir[R_SUM1].s = 2;' \
  'fx.ir[R_SUM2].s = 3;' \
  'fx.ir[R_SUM1].s = 4;' \
  'view.spadjust = 16;' \
  'fx.ir[R_SUM1].s = 255;' \
  'view.spadjust = 1008;' \
  'view.spadjust = 4;' \
  'view.spadjust = 8;' \
  'view.spadjust = 12;' \
  'view.spadjust = 1024;' \
  'view.spadjust = 32;' \
  'view.spadjust = 992;' \
  'fx.ir[R_SUM1].s = 1;' \
  'fx.ir[R_SUM1].prev = REGSP(RID_INIT, SPS_NONE);' \
  'fx.ir[R_PRECOND].s = 2;' \
  'fx.ir[R_LOOP].s = 2;' \
  'fx.ir[R_XPOLL].s = 2;' \
  'setir(R_END, IR_RENAME, IRT_NIL, R_SUM1, 0);' \
  'fx.ir[R_END].r = RID_MAX_GPR;' \
  'fx.ir[R_END].s = 2;' \
  'fx.snap[1].nent = fx.T.nsnapmap;' \
  'view.nk = fx.T.nk;' \
  'view.proto_sizebc = fixture_pt->sizebc;' \
  'view.root_topslot = fixture_pt->framesize;' \
  'fx.snap[1].mapofs = 1;' \
  'fx.snap[1].nent = 1;' \
  'fixture_pt->framesize+2u+LJ_FR2' \
  'fixture_pt->framesize-1u' \
  'SNAP(1, SNAP_FRAME|SNAP_NORESTORE, REF_NIL)' \
  'fx.snapmap[6] = SNAP(2, SNAP_NORESTORE, R_SUM1);' \
  'fx.snapmap[6] = SNAP(2, 0, R_BODY1);' \
  'fx.snap[3].ref = R_PRECOND;' \
  'fx.snapmap[6] = SNAP(2, 0, K_ZERO-1u);' \
  'setir(K_STEP, IR_KNUM, IRT_NUM, 0, 0);' \
  'view.nk = 0;' \
  'view.nk = REF_TRUE+1u;' \
  'set_snapshot_payload(2, fixture_snapshot_pc, 1);' \
  '(uintptr_t)proto_bc(fixture_pt)+1u' \
  'proto_bc(fixture_pt)+fixture_pt->sizebc'; do
  grep -F "$required" "$root/tests/t-arm64-jit-ir-admission.c" >/dev/null || {
    echo "ARM64 post-RA mutation coverage changed: $required" >&2
    exit 1
  }
done

# ARM64 records the already-lowered full XPOLL only for root LOOP traces.
# Side/stitch recording and non-loop native entry remain independently closed.
test "$(grep -Fc '#if (LJ_TARGET_X64 || LJ_TARGET_ARM64) && LJ_GC64' \
  "$root/src/lj_record.c")" -eq 3
test "$(grep -Fc '#if (LJ_TARGET_X64 || LJ_TARGET_ARM64) && LJ_GC64' \
  "$root/src/lj_opt_loop.c")" -eq 2
grep -F 'emit_getgl32acq(as, gate, gc2.jit_phase_gate);' \
  "$root/src/lj_asm_arm64.h" >/dev/null
grep -F 'emit_gettg32(as, gate, poll);' "$root/src/lj_asm_arm64.h" >/dev/null
grep -F 'emit_gettg32(as, profile, profile_request);' \
  "$root/src/lj_asm_arm64.h" >/dev/null
grep -A24 '^void LJ_FASTCALL lj_trace_hot' "$root/src/lj_trace.c" | \
  grep -F '#if LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED' >/dev/null
grep -A10 '^void lj_trace_ins' "$root/src/lj_trace.c" | \
  grep -F '#if LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED' >/dev/null
grep -F '#if LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED' \
  "$root/src/vm_arm64.dasc" >/dev/null

# arm64e/BTI compilation catches pointer-auth target drift without executing
# any admitted trace.
# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O0 -Wall -Wextra -Werror -arch arm64e \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  -c "$root/src/lj_asm.c" -o "$audit_object"

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  "$root/tests/t-arm64-jit-ir-admission.c" "$archive" -lm -pthread \
  -o "$fixture"
"$fixture"

echo "arm64_jit_ir_admission_contract OK: exact integer, mixed-NUM, fixed-half, dynamic-step and ADD_LT/ADD_LE/SUB_GT dynamic-accumulator pure-NUM LOOP/FORL grammars, bounded integer spills, and FPR-only NUM layouts verified"
