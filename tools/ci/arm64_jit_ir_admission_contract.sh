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
asm_source=$root/src/lj_asm.c
admit_source=$root/src/lj_asm_arm64_admit.h
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-ir-admission.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

fixture=$tmpdir/t-arm64-jit-ir-admission
classifier=$tmpdir/classifier.txt
semantic_region=$tmpdir/semantic-region.txt
trace_asm=$tmpdir/trace-asm.txt
call_region=$tmpdir/call-region.txt
value_region=$tmpdir/value-region.txt
postra_region=$tmpdir/postra-region.txt
arm64_cnew_region=$tmpdir/arm64-cnew-region.txt
positive_region=$tmpdir/positive-region.txt
dynamic_forl_region=$tmpdir/dynamic-forl-region.txt
dynamic_forl_tests=$tmpdir/dynamic-forl-tests.txt
numhalf_region=$tmpdir/numhalf-region.txt
numstep_region=$tmpdir/numstep-region.txt
numacc_region=$tmpdir/numacc-region.txt
numacc_intstep_region=$tmpdir/numacc-intstep-region.txt
numacc_intlimit_region=$tmpdir/numacc-intlimit-region.txt
numacc_intx_region=$tmpdir/numacc-intx-region.txt
numacc_intlimit_semantic_test_region=$tmpdir/numacc-intlimit-semantic-test.txt
numacc_intlimit_postra_test_region=$tmpdir/numacc-intlimit-postra-test.txt
numacc_intx_semantic_test_region=$tmpdir/numacc-intx-semantic-test.txt
numacc_intx_postra_test_region=$tmpdir/numacc-intx-postra-test.txt
postra_addgt_region=$tmpdir/postra-addgt-region.txt
semantic_addgt_region=$tmpdir/semantic-addgt-region.txt
postra_addge_region=$tmpdir/postra-addge-region.txt
semantic_addge_region=$tmpdir/semantic-addge-region.txt
selector_add_region=$tmpdir/selector-add-region.txt
numdynamic_sub_helper=$tmpdir/numdynamic-sub-helper.txt
numdynamic_mul_helper=$tmpdir/numdynamic-mul-helper.txt
numdynamic_div_helper=$tmpdir/numdynamic-div-helper.txt
numacc_main_region=$tmpdir/numacc-main-region.txt
postra_subge_region=$tmpdir/postra-subge-region.txt
semantic_subge_region=$tmpdir/semantic-subge-region.txt
selector_sub_region=$tmpdir/selector-sub-region.txt
postra_mullt_region=$tmpdir/postra-mullt-region.txt
semantic_mullt_region=$tmpdir/semantic-mullt-region.txt
postra_mulle_region=$tmpdir/postra-mulle-region.txt
semantic_mulle_region=$tmpdir/semantic-mulle-region.txt
selector_mul_region=$tmpdir/selector-mul-region.txt
numacc_mulle_fixture_region=$tmpdir/numacc-mulle-fixture-region.txt
postra_divlt_region=$tmpdir/postra-divlt-region.txt
semantic_divlt_region=$tmpdir/semantic-divlt-region.txt
postra_divle_region=$tmpdir/postra-divle-region.txt
semantic_divle_region=$tmpdir/semantic-divle-region.txt
postra_divgt_region=$tmpdir/postra-divgt-region.txt
semantic_divgt_region=$tmpdir/semantic-divgt-region.txt
postra_divge_region=$tmpdir/postra-divge-region.txt
semantic_divge_region=$tmpdir/semantic-divge-region.txt
selector_div_region=$tmpdir/selector-div-region.txt
numacc_divlt_fixture_region=$tmpdir/numacc-divlt-fixture-region.txt
numacc_divle_fixture_region=$tmpdir/numacc-divle-fixture-region.txt
numacc_divgt_fixture_region=$tmpdir/numacc-divgt-fixture-region.txt
numacc_divge_fixture_region=$tmpdir/numacc-divge-fixture-region.txt
pauth_macros=$tmpdir/macros-arm64e.txt
audit_object=$tmpdir/lj_asm-arm64e.o
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT'
pauth_xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR"

test -f "$archive" && test -f "$asm_object" || {
  echo "ARM64 IR admission contract requires an existing experimental build" >&2
  exit 1
}
test "$asm_object" -nt "$asm_source" &&
test "$asm_object" -nt "$admit_source" || {
  echo "ARM64 IR admission object is stale relative to its sources" >&2
  exit 1
}
ar -p "$archive" lj_asm.o | cmp - "$asm_object" || {
  echo "ARM64 IR admission archive does not contain current lj_asm.o" >&2
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
  "$admit_source" >"$classifier"
test -s "$classifier"

awk '/^int lj_asm_arm64_ir_admit/ { copying = 1 }
     copying { print }
     copying && /^\/\* -- Assembler state and common macros/ { exit }' \
  "$admit_source" >"$semantic_region"
test -s "$semantic_region"

awk '/^static int arm64_ir_int_value_op/ { copying = 1 }
     copying { print }
     copying && /^static int arm64_postra_int_value/ { exit }' \
  "$admit_source" >"$value_region"
grep -F 'case IR_SLOAD: case IR_ADDOV: case IR_SUBOV: case IR_MULOV:' \
  "$value_region" >/dev/null
grep -F 'case IR_ADD:' "$value_region" >/dev/null
grep -F 'return allow_add;' "$value_region" >/dev/null
grep -F 'static int arm64_ir_num_value_op(IROp op, int allow_sub, int allow_mul,' \
  "$value_region" >/dev/null
grep -F 'int allow_div)' "$value_region" >/dev/null
grep -F 'return op == IR_SLOAD || op == IR_ADD || (allow_sub && op == IR_SUB) ||' \
  "$value_region" >/dev/null
grep -F '(allow_mul && op == IR_MUL) || (allow_div && op == IR_DIV);' \
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
  "$admit_source" >"$postra_region"
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

# The variable-stop/variable-step FORL fixture pins the recorder's exact
# direction and overflow proof, including the sole scoped IR_USE.
awk '/^static void make_dynamic_forl_trace/ { copying = 1 }
     copying { print }
     copying && /^static void make_numeric_trace/ { exit }' \
  "$root/tests/t-arm64-jit-ir-admission.c" >"$dynamic_forl_region"
for required in \
  'V_R_STOP, IR_SLOAD, IRT_INT, idxslot+FORL_STOP' \
  'V_R_STEP, IR_SLOAD, IRT_INT, idxslot+FORL_STEP' \
  'V_R_DIRECTION, direction, IRT_INT|IRT_GUARD' \
  'V_R_OVERFLOW, IR_ADDOV, IRT_INT|IRT_GUARD' \
  'V_R_USE, IR_USE, IRT_INT, V_R_OVERFLOW, 0' \
  'V_R_IDX_PRE, IR_ADD, IRT_INT|IRT_ISPHI' \
  'V_R_IDX_BODY, IR_ADD, IRT_INT|IRT_ISPHI' \
  'SNAP(8, SNAP_NORESTORE, V_R_STEP)' \
  'fx.T.nsnapmap = 32;' \
  'fx.T.nins = V_R_SEMANTIC_END;'; do
  grep -F "$required" "$dynamic_forl_region" >/dev/null || {
    echo "ARM64 dynamic FORL fixture changed: $required" >&2
    exit 1
  }
done
test "$(grep -c 'IR_USE' "$dynamic_forl_region")" -eq 1

awk '/^static void test_dynamic_forl_positive_and_negative/ { copying = 1 }
     copying { print }
     copying && /^static void test_postra_spill_layout/ { exit }' \
  "$root/tests/t-arm64-jit-ir-admission.c" >"$dynamic_forl_tests"
for required in \
  'fx.ir[V_R_STEP].op2 = IRSLOAD_TYPECHECK|IRSLOAD_INHERIT;' \
  'fx.ir[V_R_DIRECTION].o = IR_GT;' \
  'fx.ir[V_R_OVERFLOW].op1 = V_R_STOP;' \
  'fx.ir[V_R_USE].op1 = V_R_STOP;' \
  'fx.ir[V_R_USE].s = SPS_FIRST;' \
  'fx.ir[V_R_OVERFLOW].r = RID_INIT;' \
  'fx.ir[V_R_STEP].r = RID_D0;' \
  'fx.ir[V_R_STEP].r = fx.ir[V_R_STOP].r;' \
  'fx.ir[V_R_IDX].r = fx.ir[V_R_STEP].r;' \
  'fx.ir[V_R_SUM].r = fx.ir[V_R_IDX].r;' \
  'fx.ir[V_R_OVERFLOW].r = fx.ir[V_R_STEP].r;' \
  'fx.ir[V_R_IDX_BODY].r = RID_X26;' \
  'fx.ir[V_R_SUM_BODY].r = RID_X26;' \
  'fx.ir[V_R_SUM_PHI].r = fx.ir[V_R_IDX_PHI].r;' \
  'fx.snapmap[4] = SNAP(8, SNAP_NORESTORE, V_R_STOP);'; do
  grep -F "$required" "$dynamic_forl_tests" >/dev/null || {
    echo "ARM64 dynamic FORL mutation coverage changed: $required" >&2
    exit 1
  }
done
for required in \
  'test_dynamic_forl_positive_and_negative(J);' \
  'test_dynamic_forl_postra_layout(J);'; do
  grep -F "$required" "$root/tests/t-arm64-jit-ir-admission.c" >/dev/null
done

# Keep the production check ahead of IR scratch growth, compact-trace
# allocation and mcode reservation/publication.
awk '/^void lj_asm_trace\(/ { copying = 1 }
     copying { print }
     copying && /^#if LJ_TARGET_ARM64 && defined\(LJ_ARM64_EMIT_TEST_HELPERS\)/ {
       exit
     }' "$asm_source" >"$trace_asm"
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
  'setir(N_R_X_PRE, IR_CONV, IRT_NUM|IRT_ISPHI,' \
  'expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_CONV);' \
  'setir(N_R_X_PRE, IR_DIV, IRT_NUM|IRT_ISPHI, N_R_STEP, N_R_X);' \
  'expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_DIV);' \
  'setir(N_R_X_PRE, IR_MUL, IRT_NUM|IRT_ISPHI, N_R_STEP, N_R_X);' \
  'expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_MUL);' \
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
  'ARM64_NUMDYN_ADD_LT, ARM64_NUMDYN_ARGS_NUM);' \
  '} else if (constant_profile == ARM64_IR_KPROFILE_INT) {' \
  '!arm64_postra_numstep_shape(view, semantic_nins)' \
  '} else if (constant_profile == ARM64_IR_KPROFILE_INT &&' \
  'T->nk == REF_TRUE) {' \
  '!arm64_ir_numdynamic_kernel(T, 4, 3, 2, ARM64_NUMDYN_ADD_LT,' \
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

# The dynamic-accumulator fixture crosses twelve exact arithmetic/comparison
# profiles with four argument kinds: all-NUM, invariant INT step widened once,
# INT accumulator widened plus checked once, and the ADD_LT-only invariant INT
# limit widened once. Trace and prototype constant sets remain empty.
awk '/^static unsigned numacc_fixture_full_shape\(/ { copying = 1 }
     copying { print }
     copying && /^static LJArm64IRReject expect_reject\(/ { exit }' \
  "$root/tests/t-arm64-jit-ir-admission.c" >"$numacc_region"
test -s "$numacc_region"
test "$(grep -c 'IR_KNUM' "$numacc_region" || true)" -eq 0
test "$(grep -c 'IR_KINT' "$numacc_region" || true)" -eq 0
test "$(grep -c 'IR_SLOAD' "$numacc_region")" -eq 12
test "$(grep -c 'profile->recurrence_op' "$numacc_region")" -eq 8
test "$(grep -c 'profile->precondition_op' "$numacc_region")" -eq 4
test "$(grep -c 'profile->body_op' "$numacc_region")" -eq 4
test "$(grep -c 'IR_LOOP' "$numacc_region")" -eq 4
test "$(grep -c 'IR_XPOLL' "$numacc_region")" -eq 4
test "$(grep -c 'IR_PHI' "$numacc_region")" -eq 4
test "$(grep -c 'IR_CONV' "$numacc_region")" -eq 4

awk '/^static void make_numacc_intstep_trace\(/ { copying = 1 }
     copying { print }
     copying && /^static void make_numacc_intlimit_trace\(/ { exit }' \
  "$root/tests/t-arm64-jit-ir-admission.c" >"$numacc_intstep_region"
test -s "$numacc_intstep_region"
test "$(grep -c 'IR_SLOAD' "$numacc_intstep_region")" -eq 3
test "$(grep -c 'IR_CONV' "$numacc_intstep_region")" -eq 1
test "$(grep -c 'IR_LOOP' "$numacc_intstep_region")" -eq 1
test "$(grep -c 'IR_XPOLL' "$numacc_intstep_region")" -eq 1
test "$(grep -c 'IR_PHI' "$numacc_intstep_region")" -eq 1

awk '/^static void make_numacc_intlimit_trace\(/ { copying = 1 }
     copying { print }
     copying && /^static void make_numacc_intx_trace\(/ { exit }' \
  "$root/tests/t-arm64-jit-ir-admission.c" >"$numacc_intlimit_region"
test -s "$numacc_intlimit_region"
test "$(grep -c 'IR_SLOAD' "$numacc_intlimit_region")" -eq 3
test "$(grep -c 'IR_CONV' "$numacc_intlimit_region")" -eq 1
test "$(grep -c 'IR_LOOP' "$numacc_intlimit_region")" -eq 1
test "$(grep -c 'IR_XPOLL' "$numacc_intlimit_region")" -eq 1
test "$(grep -c 'IR_PHI' "$numacc_intlimit_region")" -eq 1

awk '/^static void make_numacc_intx_trace\(/ { copying = 1 }
     copying { print }
     copying && /^static LJArm64IRReject expect_reject\(/ { exit }' \
  "$root/tests/t-arm64-jit-ir-admission.c" >"$numacc_intx_region"
test -s "$numacc_intx_region"
test "$(grep -c 'IR_SLOAD' "$numacc_intx_region")" -eq 3
test "$(grep -c 'IR_CONV' "$numacc_intx_region")" -eq 2
test "$(grep -c 'IR_LOOP' "$numacc_intx_region")" -eq 1
test "$(grep -c 'IR_XPOLL' "$numacc_intx_region")" -eq 1
test "$(grep -c 'IR_PHI' "$numacc_intx_region")" -eq 1

awk '/^static void test_numacc_intlimit_positive_and_negative\(/ {
       copying = 1
     }
     copying { print }
     copying && /^static void test_numacc_intx_positive_and_negative\(/ { exit }' \
  "$root/tests/t-arm64-jit-ir-admission.c" \
  >"$numacc_intlimit_semantic_test_region"
test -s "$numacc_intlimit_semantic_test_region"

awk '/^static void test_numacc_intlimit_postra_layout\(/ { copying = 1 }
     copying { print }
     copying && /^static void test_numacc_intx_postra_layout\(/ { exit }' \
  "$root/tests/t-arm64-jit-ir-admission.c" \
  >"$numacc_intlimit_postra_test_region"
test -s "$numacc_intlimit_postra_test_region"

awk '/^static void test_numacc_intx_positive_and_negative\(/ { copying = 1 }
     copying { print }
     copying && /^static void test_numacc_shape_cross_product\(/ { exit }' \
  "$root/tests/t-arm64-jit-ir-admission.c" \
  >"$numacc_intx_semantic_test_region"
test -s "$numacc_intx_semantic_test_region"

awk '/^static void test_numacc_intx_postra_layout\(/ { copying = 1 }
     copying { print }
     copying && /^static void test_postra_spill_layout\(/ { exit }' \
  "$root/tests/t-arm64-jit-ir-admission.c" \
  >"$numacc_intx_postra_test_region"
test -s "$numacc_intx_postra_test_region"

for required in \
  'ARM64_NUMDYN_ADD_LT = 1u,' \
  'ARM64_NUMDYN_ADD_LE = 2u,' \
  'ARM64_NUMDYN_SUB_GT = 3u,' \
  'ARM64_NUMDYN_SUB_GE = 4u,' \
  'ARM64_NUMDYN_ADD_GT = 5u,' \
  'ARM64_NUMDYN_ADD_GE = 6u,' \
  'ARM64_NUMDYN_MUL_LT = 7u,' \
  'ARM64_NUMDYN_MUL_LE = 8u,' \
  'ARM64_NUMDYN_DIV_LT = 9u,' \
  'ARM64_NUMDYN_DIV_LE = 10u,' \
  'ARM64_NUMDYN_DIV_GT = 11u,' \
  'ARM64_NUMDYN_DIV_GE = 12u' \
  'ARM64_NUMDYN_ARGS_NUM = 1u,' \
  'ARM64_NUMDYN_ARGS_INT_STEP = 2u,' \
  'ARM64_NUMDYN_ARGS_INT_LIMIT = 3u,' \
  'ARM64_NUMDYN_ARGS_INT_X = 4u' \
  'static int arm64_numdynamic_is_sub(unsigned grammar_profile)' \
  'return grammar_profile == ARM64_NUMDYN_SUB_GT ||' \
  'grammar_profile == ARM64_NUMDYN_SUB_GE;' \
  'static int arm64_numdynamic_is_mul(unsigned grammar_profile)' \
  'return grammar_profile == ARM64_NUMDYN_MUL_LT ||' \
  'grammar_profile == ARM64_NUMDYN_MUL_LE;' \
  'static int arm64_numdynamic_is_div(unsigned grammar_profile)' \
  'return grammar_profile == ARM64_NUMDYN_DIV_LT ||' \
  'grammar_profile == ARM64_NUMDYN_DIV_LE ||' \
  'grammar_profile == ARM64_NUMDYN_DIV_GT ||' \
  'grammar_profile == ARM64_NUMDYN_DIV_GE;' \
  'ARM64_NUMACC_R_X = ARM64_NUMSTEP_R_X,' \
  'ARM64_NUMACC_R_STEP = ARM64_NUMSTEP_R_STEP,' \
  'ARM64_NUMACC_SEMANTIC_NINS = ARM64_NUMSTEP_SEMANTIC_NINS' \
  'ARM64_NUMACC_INTSTEP_R_X = REF_FIRST,' \
  'ARM64_NUMACC_INTSTEP_R_STEP_INT,' \
  'ARM64_NUMACC_INTSTEP_R_STEP_NUM,' \
  'ARM64_NUMACC_INTSTEP_R_X_PRE,' \
  'ARM64_NUMACC_INTSTEP_R_LIMIT,' \
  'ARM64_NUMACC_INTSTEP_R_PRE_GUARD,' \
  'ARM64_NUMACC_INTSTEP_R_LOOP,' \
  'ARM64_NUMACC_INTSTEP_R_XPOLL,' \
  'ARM64_NUMACC_INTSTEP_R_X_BODY,' \
  'ARM64_NUMACC_INTSTEP_R_BODY_GUARD,' \
  'ARM64_NUMACC_INTSTEP_R_X_PHI,' \
  'ARM64_NUMACC_INTSTEP_SEMANTIC_NINS' \
  'ARM64_NUMACC_INTLIMIT_R_X = REF_FIRST,' \
  'ARM64_NUMACC_INTLIMIT_R_STEP,' \
  'ARM64_NUMACC_INTLIMIT_R_X_PRE,' \
  'ARM64_NUMACC_INTLIMIT_R_LIMIT_INT,' \
  'ARM64_NUMACC_INTLIMIT_R_LIMIT_NUM,' \
  'ARM64_NUMACC_INTLIMIT_R_PRE_GUARD,' \
  'ARM64_NUMACC_INTLIMIT_R_LOOP,' \
  'ARM64_NUMACC_INTLIMIT_R_XPOLL,' \
  'ARM64_NUMACC_INTLIMIT_R_X_BODY,' \
  'ARM64_NUMACC_INTLIMIT_R_BODY_GUARD,' \
  'ARM64_NUMACC_INTLIMIT_R_X_PHI,' \
  'ARM64_NUMACC_INTLIMIT_SEMANTIC_NINS' \
  'ARM64_NUMACC_INTX_R_X_INT = REF_FIRST,' \
  'ARM64_NUMACC_INTX_R_STEP,' \
  'ARM64_NUMACC_INTX_R_X_NUM,' \
  'ARM64_NUMACC_INTX_R_X_PRE,' \
  'ARM64_NUMACC_INTX_R_LIMIT,' \
  'ARM64_NUMACC_INTX_R_PRE_GUARD,' \
  'ARM64_NUMACC_INTX_R_LOOP,' \
  'ARM64_NUMACC_INTX_R_XPOLL,' \
  'ARM64_NUMACC_INTX_R_X_CHECK,' \
  'ARM64_NUMACC_INTX_R_X_BODY,' \
  'ARM64_NUMACC_INTX_R_BODY_GUARD,' \
  'ARM64_NUMACC_INTX_R_X_PHI,' \
  'ARM64_NUMACC_INTX_SEMANTIC_NINS' \
  'static int arm64_numacc_snapshots(const SnapShot *snap,' \
  'static const uint8_t nslots[5] = { 5, 6, 5, 5, 5 };' \
  'static const uint8_t pcpos[5] = { 6, 2, 11, 6, 11 };' \
  'SNAP(2, 0, ARM64_NUMACC_R_X_PRE),' \
  'SNAP(5, 0, ARM64_NUMACC_R_X_PRE),' \
  'SNAP(2, 0, ARM64_NUMACC_R_X_BODY)' \
  'SNAP(2, 0, ARM64_NUMACC_INTSTEP_R_X_PRE),' \
  'SNAP(5, 0, ARM64_NUMACC_INTSTEP_R_X_PRE),' \
  'SNAP(2, 0, ARM64_NUMACC_INTSTEP_R_X_BODY)' \
  'SNAP(2, 0, ARM64_NUMACC_INTLIMIT_R_X_PRE),' \
  'SNAP(5, 0, ARM64_NUMACC_INTLIMIT_R_X_PRE),' \
  'SNAP(2, 0, ARM64_NUMACC_INTLIMIT_R_X_BODY)' \
  'SNAP(2, 0, ARM64_NUMACC_INTX_R_X_PRE),' \
  'SNAP(5, 0, ARM64_NUMACC_INTX_R_X_PRE),' \
  'SNAP(2, 0, ARM64_NUMACC_INTX_R_X_BODY)' \
  'nsnap != 5 || nsnapmap != 15 || proto_sizebc != 13' \
  'case ARM64_NUMDYN_ARGS_NUM: kindidx = 0; break;' \
  'case ARM64_NUMDYN_ARGS_INT_STEP: kindidx = 1; break;' \
  'case ARM64_NUMDYN_ARGS_INT_LIMIT: kindidx = 2; break;' \
  'case ARM64_NUMDYN_ARGS_INT_X: kindidx = 3; break;' \
  'default: return 0;' \
  'static int arm64_postra_numdynamic_kernel(const LJArm64PostRAView *view,' \
  'IRRef xslot, IRRef stepslot, IRRef limitslot,' \
  'unsigned grammar_profile, unsigned args_kind)' \
  'if (args_kind == ARM64_NUMDYN_ARGS_NUM) {' \
  '} else if (args_kind == ARM64_NUMDYN_ARGS_INT_STEP) {' \
  '} else if (args_kind == ARM64_NUMDYN_ARGS_INT_LIMIT &&' \
  'grammar_profile == ARM64_NUMDYN_ADD_LT) {' \
  '} else if (args_kind == ARM64_NUMDYN_ARGS_INT_X) {' \
  'if (grammar_profile == ARM64_NUMDYN_ADD_LT) {' \
  '} else if (grammar_profile == ARM64_NUMDYN_ADD_LE) {' \
  '} else if (grammar_profile == ARM64_NUMDYN_ADD_GT) {' \
  '} else if (grammar_profile == ARM64_NUMDYN_ADD_GE) {' \
  '} else if (grammar_profile == ARM64_NUMDYN_SUB_GT) {' \
  '} else if (grammar_profile == ARM64_NUMDYN_SUB_GE) {' \
  '} else if (grammar_profile == ARM64_NUMDYN_MUL_LT) {' \
  '} else if (grammar_profile == ARM64_NUMDYN_MUL_LE) {' \
  '} else if (grammar_profile == ARM64_NUMDYN_DIV_LT) {' \
  '} else if (grammar_profile == ARM64_NUMDYN_DIV_LE) {' \
  '} else if (grammar_profile == ARM64_NUMDYN_DIV_GT) {' \
  '} else if (grammar_profile == ARM64_NUMDYN_DIV_GE) {' \
  'recurrence_op = IR_SUB;' \
  'first_left = xref;' \
  'first_right = stepref;' \
  'preop = IR_GE;' \
  'bodyop = IR_LE;' \
  'preop = IR_LT;' \
  'bodyop = IR_GT;' \
  'ARM64_NUMDYN_POSTRA_INS(stepintref, IR_SLOAD,' \
  'IRT_INT|IRT_GUARD, stepslot, IRSLOAD_TYPECHECK)' \
  'ARM64_NUMDYN_POSTRA_INS(stepref, IR_CONV,' \
  'IRT_NUM, stepintref, IRCONV_NUM_INT)' \
  'ARM64_NUMDYN_POSTRA_INS(limitintref, IR_SLOAD,' \
  'IRT_INT|IRT_GUARD, limitslot, IRSLOAD_TYPECHECK)' \
  'ARM64_NUMDYN_POSTRA_INS(limitref, IR_CONV,' \
  'IRT_NUM, limitintref, IRCONV_NUM_INT)' \
  'ARM64_NUMDYN_POSTRA_INS(xintref, IR_SLOAD,' \
  'IRT_INT|IRT_GUARD, xslot, IRSLOAD_TYPECHECK)' \
  'ARM64_NUMDYN_POSTRA_INS(xref, IR_CONV,' \
  'IRT_NUM, xintref, IRCONV_NUM_INT)' \
  'ARM64_NUMDYN_POSTRA_INS(xcheckref, IR_CONV,' \
  'IRT_INT|IRT_GUARD, xpreref, IRCONV_INT_NUM|IRCONV_CHECK)' \
  'ARM64_NUMDYN_POSTRA_INS(xpreref, recurrence_op,' \
  'IRT_NUM|IRT_ISPHI, first_left, first_right)' \
  'ARM64_NUMDYN_POSTRA_INS(xbodyref, recurrence_op,' \
  'step_int.s != SPS_NONE || step_int.r >= RID_MAX_GPR ||' \
  '!rset_test(RSET_GPR, step_int.r) || step.s != SPS_NONE ||' \
  'step.r < RID_MIN_FPR || step.r >= RID_MAX_FPR ||' \
  '!rset_test(RSET_FPR, step.r)' \
  'limit_int.s != SPS_NONE || limit_int.r >= RID_MAX_GPR ||' \
  '!rset_test(RSET_GPR, limit_int.r) || limit.s != SPS_NONE ||' \
  'limit.r < RID_MIN_FPR || limit.r >= RID_MAX_FPR ||' \
  '!rset_test(RSET_FPR, limit.r)' \
  'return arm64_postra_numdynamic_kernel(view, 2, 4, 3,' \
  'static int arm64_postra_numacc_shape(const LJArm64PostRAView *view,' \
  'static unsigned arm64_numacc_grammar_profile(const BCIns *proto_bc,' \
  'bc_op(recurrence) == BC_ADDVV && bc_a(recurrence) == 3' \
  'return ARM64_NUMDYN_ADD_LT;' \
  'return ARM64_NUMDYN_ADD_LE;' \
  'return ARM64_NUMDYN_ADD_GT;' \
  'return ARM64_NUMDYN_ADD_GE;' \
  'bc_op(recurrence) == BC_SUBVV && bc_a(recurrence) == 3' \
  'bc_op(compare) == BC_ISGE && bc_a(compare) == 4' \
  'return ARM64_NUMDYN_SUB_GT;' \
  'bc_op(compare) == BC_ISGT && bc_a(compare) == 4' \
  'return ARM64_NUMDYN_SUB_GE;' \
  'bc_op(recurrence) == BC_MULVV && bc_a(recurrence) == 3' \
  'return ARM64_NUMDYN_MUL_LT;' \
  'return ARM64_NUMDYN_MUL_LE;' \
  'bc_op(recurrence) == BC_DIVVV && bc_a(recurrence) == 3' \
  'return ARM64_NUMDYN_DIV_LT;' \
  'return ARM64_NUMDYN_DIV_LE;' \
  'return ARM64_NUMDYN_DIV_GT;' \
  'return ARM64_NUMDYN_DIV_GE;' \
  'unsigned grammar_profile = arm64_numacc_grammar_profile(' \
  'view->root_topslot != 5 || view->proto_sizebc != 13 ||' \
  'grammar_profile == 0 ||' \
  'semantic_nins != (args_kind == ARM64_NUMDYN_ARGS_NUM ?' \
  'args_kind == ARM64_NUMDYN_ARGS_INT_STEP ?' \
  'args_kind == ARM64_NUMDYN_ARGS_INT_LIMIT ?' \
  'ARM64_NUMACC_INTLIMIT_SEMANTIC_NINS :' \
  'ARM64_NUMACC_INTX_SEMANTIC_NINS) ||' \
  '!arm64_numacc_snapshots(view->snap, view->snapmap,' \
  'else if (view->proto_sizebc == 13) {' \
  '!arm64_postra_numacc_shape(view, semantic_nins,' \
  'ARM64_NUMDYN_ARGS_NUM)' \
  'ARM64_NUMDYN_ARGS_INT_STEP)' \
  'ARM64_NUMDYN_ARGS_INT_X)' \
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
  'IRRef stepslot, IRRef limitslot, unsigned grammar_profile,' \
  'unsigned args_kind)' \
  'ARM64_NUMDYN_INS(stepintref, IR_SLOAD,' \
  'ARM64_NUMDYN_INS(stepref, IR_CONV,' \
  'IRT_NUM, stepintref, IRCONV_NUM_INT)' \
  'ARM64_NUMDYN_INS(xintref, IR_SLOAD,' \
  'ARM64_NUMDYN_INS(xref, IR_CONV,' \
  'IRT_NUM, xintref, IRCONV_NUM_INT)' \
  'ARM64_NUMDYN_INS(xcheckref, IR_CONV,' \
  'IRT_INT|IRT_GUARD, xpreref, IRCONV_INT_NUM|IRCONV_CHECK)' \
  'ARM64_NUMDYN_INS(xpreref, recurrence_op,' \
  'ARM64_NUMDYN_INS(xbodyref, recurrence_op,' \
  'bc_b(ins) != 3 || bc_c(ins) != 4)' \
  'static int arm64_ir_numacc_shape(const jit_State *J, const GCtrace *T,' \
  'IRRef semantic_nins = args_kind == ARM64_NUMDYN_ARGS_NUM ?' \
  'args_kind == ARM64_NUMDYN_ARGS_INT_STEP ?' \
  'args_kind == ARM64_NUMDYN_ARGS_INT_LIMIT ?' \
  'ARM64_NUMACC_INTLIMIT_SEMANTIC_NINS :' \
  'ARM64_NUMACC_INTX_SEMANTIC_NINS;' \
  'T->nk != REF_TRUE || T->nins != semantic_nins' \
  '!arm64_ir_numacc_bytecode(pt, trace_startpc_acq((GCtrace *)T),' \
  '!arm64_numacc_snapshots(T->snap, T->snapmap, T->nsnap,' \
  '!arm64_ir_numdynamic_kernel(T, 2, 4, 3, grammar_profile,' \
  'args_kind)' \
  'IROp recurrence_op = arm64_numdynamic_is_sub(grammar_profile) ? IR_SUB :' \
  'arm64_numdynamic_is_mul(grammar_profile) ? IR_MUL :' \
  'arm64_numdynamic_is_div(grammar_profile) ? IR_DIV : IR_ADD;' \
  'allow_num_sub = arm64_numdynamic_is_sub(numdynamic_profile);' \
  'allow_num_mul = arm64_numdynamic_is_mul(numdynamic_profile);' \
  'allow_num_div = arm64_numdynamic_is_div(numdynamic_profile);' \
  'case IR_CONV:' \
  'numdynamic_args_kind = ARM64_NUMDYN_ARGS_INT_STEP;' \
  'numdynamic_args_kind = ARM64_NUMDYN_ARGS_INT_LIMIT;' \
  'numdynamic_args_kind = ARM64_NUMDYN_ARGS_INT_X;' \
  'if (!allow_num_sub || startop != BC_LOOP ||' \
  'if (!allow_num_sub || irt_type(ins.t) != IRT_NUM)' \
  'if (!allow_num_mul || startop != BC_LOOP ||' \
  'if (!allow_num_mul || irt_type(ins.t) != IRT_NUM)' \
  'if (!allow_num_div || startop != BC_LOOP ||' \
  'if (!allow_num_div || irt_type(ins.t) != IRT_NUM)' \
  '!arm64_ir_num_ref(T, ref, snapref, allow_num_sub,' \
  'allow_num_mul, allow_num_div)' \
  'else if (pt->sizebc == 13) {' \
  '!arm64_ir_numacc_shape(J, T, pt, firstphi,' \
  'ARM64_NUMDYN_ARGS_NUM, reject)' \
  'numdynamic_args_kind, reject)'; do
  grep -F "$required" "$classifier" >/dev/null || {
    echo "ARM64 dynamic-accumulator NUM production contract changed: $required" >&2
    exit 1
  }
done
for required in \
  'if (numdynamic_profile == 0 || slot != SPS_NONE)' \
  'numdynamic_args_kind == ARM64_NUMDYN_ARGS_NUM &&' \
  'ins.t.irt == IRT_NUM && ins.op2 == IRCONV_NUM_INT &&' \
  'ins.r >= RID_MIN_FPR && ins.r < RID_MAX_FPR &&' \
  'ref == ARM64_NUMACC_INTSTEP_R_STEP_NUM &&' \
  'ins.op1 == ARM64_NUMACC_INTSTEP_R_STEP_INT) {' \
  'numdynamic_args_kind = ARM64_NUMDYN_ARGS_INT_STEP;' \
  'numdynamic_profile == ARM64_NUMDYN_ADD_LT &&' \
  'ref == ARM64_NUMACC_INTLIMIT_R_LIMIT_NUM &&' \
  'ins.op1 == ARM64_NUMACC_INTLIMIT_R_LIMIT_INT) {' \
  'numdynamic_args_kind = ARM64_NUMDYN_ARGS_INT_LIMIT;' \
  'ref == ARM64_NUMACC_INTX_R_X_NUM &&' \
  'ins.op1 == ARM64_NUMACC_INTX_R_X_INT) {' \
  'numdynamic_args_kind = ARM64_NUMDYN_ARGS_INT_X;' \
  'numdynamic_args_kind == ARM64_NUMDYN_ARGS_INT_X &&' \
  'ref == ARM64_NUMACC_INTX_R_X_CHECK &&' \
  'ins.t.irt == (IRT_INT|IRT_GUARD) &&' \
  'ins.op1 == ARM64_NUMACC_INTX_R_X_PRE &&' \
  'ins.op2 == (IRCONV_INT_NUM|IRCONV_CHECK) &&' \
  'ins.r < RID_MAX_GPR && rset_test(RSET_GPR, ins.r)' \
  'if (numdynamic_args_kind != ARM64_NUMDYN_ARGS_NUM) {' \
  'constant_profile != ARM64_IR_KPROFILE_INT || !suffix_is_nop ||' \
  'nrename != 0 || spadjust != 0 || highest_end != 0 ||' \
  '!arm64_postra_numacc_shape(view, semantic_nins,' \
  'numdynamic_args_kind))'; do
  grep -F "$required" "$postra_region" >/dev/null || {
    echo "ARM64 post-RA INT conversion gate changed: $required" >&2
    exit 1
  }
done
for required in \
  'if (numdynamic_profile == 0)' \
  'numdynamic_args_kind == ARM64_NUMDYN_ARGS_NUM &&' \
  'ir->t.irt == IRT_NUM && ir->op2 == IRCONV_NUM_INT)' \
  'ref == ARM64_NUMACC_INTSTEP_R_STEP_NUM &&' \
  'ir->op1 == ARM64_NUMACC_INTSTEP_R_STEP_INT) {' \
  'numdynamic_args_kind = ARM64_NUMDYN_ARGS_INT_STEP;' \
  'numdynamic_profile == ARM64_NUMDYN_ADD_LT &&' \
  'ref == ARM64_NUMACC_INTLIMIT_R_LIMIT_NUM &&' \
  'ir->op1 == ARM64_NUMACC_INTLIMIT_R_LIMIT_INT) {' \
  'numdynamic_args_kind = ARM64_NUMDYN_ARGS_INT_LIMIT;' \
  'ref == ARM64_NUMACC_INTX_R_X_NUM &&' \
  'ir->op1 == ARM64_NUMACC_INTX_R_X_INT) {' \
  'numdynamic_args_kind = ARM64_NUMDYN_ARGS_INT_X;' \
  'numdynamic_args_kind == ARM64_NUMDYN_ARGS_INT_X &&' \
  'ref == ARM64_NUMACC_INTX_R_X_CHECK &&' \
  'ir->t.irt == (IRT_INT|IRT_GUARD) &&' \
  'ir->op1 == ARM64_NUMACC_INTX_R_X_PRE &&' \
  'ir->op2 == (IRCONV_INT_NUM|IRCONV_CHECK))' \
  'return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,' \
  'IR_CONV, ir->op2);' \
  'if (numdynamic_args_kind != ARM64_NUMDYN_ARGS_NUM) {' \
  '!arm64_ir_numacc_shape(J, T, pt, firstphi,' \
  'numdynamic_args_kind, reject))'; do
  grep -F "$required" "$semantic_region" >/dev/null || {
    echo "ARM64 semantic INT conversion gate changed: $required" >&2
    exit 1
  }
done
test "$(grep -Fc 'allow_num_sub = arm64_numdynamic_is_sub(numdynamic_profile);' \
  "$classifier")" -eq 2
test "$(grep -Fc 'allow_num_mul = arm64_numdynamic_is_mul(numdynamic_profile);' \
  "$classifier")" -eq 2
test "$(grep -Fc 'allow_num_div = arm64_numdynamic_is_div(numdynamic_profile);' \
  "$classifier")" -eq 2
test "$(grep -Fc 'case IR_MUL:' "$classifier")" -eq 2
test "$(grep -Fc 'case IR_DIV:' "$classifier")" -eq 2
test "$(grep -Fc 'case IR_CONV:' "$classifier")" -eq 2
test "$(grep -Fc 'unsigned numdynamic_args_kind = ARM64_NUMDYN_ARGS_NUM;' \
  "$classifier")" -eq 2
test "$(grep -Fc 'numdynamic_args_kind = ARM64_NUMDYN_ARGS_INT_STEP;' \
  "$classifier")" -eq 2
test "$(grep -Fc 'numdynamic_args_kind = ARM64_NUMDYN_ARGS_INT_LIMIT;' \
  "$classifier")" -eq 2
test "$(grep -Fc 'numdynamic_args_kind = ARM64_NUMDYN_ARGS_INT_X;' \
  "$classifier")" -eq 2
test "$(grep -Fc 'if (numdynamic_profile == 0' "$classifier")" -eq 2
test "$(grep -Fc 'if (!allow_num_mul ||' "$classifier")" -eq 2
test "$(grep -Fc 'if (!allow_num_div ||' "$classifier")" -eq 2
test "$(grep -Fc '(allow_mul && op == IR_MUL) || (allow_div && op == IR_DIV);' \
  "$classifier")" -eq 1
test "$(grep -Fc '} else if (grammar_profile == ARM64_NUMDYN_SUB_GE) {' \
  "$classifier")" -eq 2
test "$(grep -Fc '} else if (grammar_profile == ARM64_NUMDYN_ADD_GT) {' \
  "$classifier")" -eq 2
test "$(grep -Fc '} else if (grammar_profile == ARM64_NUMDYN_ADD_GE) {' \
  "$classifier")" -eq 2
test "$(grep -Fc '} else if (grammar_profile == ARM64_NUMDYN_MUL_LT) {' \
  "$classifier")" -eq 2
test "$(grep -Fc '} else if (grammar_profile == ARM64_NUMDYN_MUL_LE) {' \
  "$classifier")" -eq 2
test "$(grep -Fc '} else if (grammar_profile == ARM64_NUMDYN_DIV_LT) {' \
  "$classifier")" -eq 2
test "$(grep -Fc '} else if (grammar_profile == ARM64_NUMDYN_DIV_LE) {' \
  "$classifier")" -eq 2
test "$(grep -Fc '} else if (grammar_profile == ARM64_NUMDYN_DIV_GT) {' \
  "$classifier")" -eq 2
test "$(grep -Fc '} else if (grammar_profile == ARM64_NUMDYN_DIV_GE) {' \
  "$classifier")" -eq 2

# ADD_GT and ADD_GE reuse the existing ADD path. Keep the dedicated SUB
# authorization helper exactly limited to the two subtraction profiles, and
# keep the independent MUL authorization exactly limited to MUL_LT/MUL_LE,
# and keep DIV authorization exact to DIV_LT/DIV_LE/DIV_GT/DIV_GE.
awk '
  /^static int arm64_numdynamic_is_sub/ { copying = 1 }
  copying { print }
  copying && /^}/ { exit }
' "$admit_source" >"$numdynamic_sub_helper"
test "$(wc -l <"$numdynamic_sub_helper" | tr -d ' ')" -eq 5
test "$(grep -Fc 'grammar_profile == ARM64_NUMDYN_' \
  "$numdynamic_sub_helper")" -eq 2
grep -F 'grammar_profile == ARM64_NUMDYN_SUB_GT ||' \
  "$numdynamic_sub_helper" >/dev/null
grep -F 'grammar_profile == ARM64_NUMDYN_SUB_GE;' \
  "$numdynamic_sub_helper" >/dev/null
for non_sub_profile in ADD_GT ADD_GE MUL_LT MUL_LE DIV_LT DIV_LE DIV_GT DIV_GE; do
  if grep -F "ARM64_NUMDYN_$non_sub_profile" \
       "$numdynamic_sub_helper" >/dev/null; then
    echo "ARM64 $non_sub_profile entered the NUM SUB authorization helper" >&2
    exit 1
  fi
done

awk '
  /^static int arm64_numdynamic_is_mul/ { copying = 1 }
  copying { print }
  copying && /^}/ { exit }
' "$admit_source" >"$numdynamic_mul_helper"
test "$(wc -l <"$numdynamic_mul_helper" | tr -d ' ')" -eq 5
test "$(grep -Fc 'grammar_profile == ARM64_NUMDYN_' \
  "$numdynamic_mul_helper")" -eq 2
grep -F 'return grammar_profile == ARM64_NUMDYN_MUL_LT ||' \
  "$numdynamic_mul_helper" >/dev/null
grep -F 'grammar_profile == ARM64_NUMDYN_MUL_LE;' \
  "$numdynamic_mul_helper" >/dev/null
for non_mul_profile in ADD_LT ADD_LE ADD_GT ADD_GE SUB_GT SUB_GE DIV_LT DIV_LE DIV_GT DIV_GE; do
  if grep -F "ARM64_NUMDYN_$non_mul_profile" \
       "$numdynamic_mul_helper" >/dev/null; then
    echo "ARM64 $non_mul_profile entered the NUM MUL authorization helper" >&2
    exit 1
  fi
done

awk '
  /^static int arm64_numdynamic_is_div/ { copying = 1 }
  copying { print }
  copying && /^}/ { exit }
' "$admit_source" >"$numdynamic_div_helper"
test "$(wc -l <"$numdynamic_div_helper" | tr -d ' ')" -eq 7
test "$(grep -Fc 'grammar_profile == ARM64_NUMDYN_' \
  "$numdynamic_div_helper")" -eq 4
grep -F 'return grammar_profile == ARM64_NUMDYN_DIV_LT ||' \
  "$numdynamic_div_helper" >/dev/null
grep -F 'grammar_profile == ARM64_NUMDYN_DIV_LE ||' \
  "$numdynamic_div_helper" >/dev/null
grep -F 'grammar_profile == ARM64_NUMDYN_DIV_GT ||' \
  "$numdynamic_div_helper" >/dev/null
grep -F 'grammar_profile == ARM64_NUMDYN_DIV_GE;' \
  "$numdynamic_div_helper" >/dev/null
for non_div_profile in ADD_LT ADD_LE ADD_GT ADD_GE SUB_GT SUB_GE MUL_LT MUL_LE; do
  if grep -F "ARM64_NUMDYN_$non_div_profile" \
       "$numdynamic_div_helper" >/dev/null; then
    echo "ARM64 $non_div_profile entered the NUM DIV authorization helper" >&2
    exit 1
  fi
done

# Pin the descending-strict ADD semantic and post-RA arms as complete scoped
# tuples, including their boundary with the following ADD_GE arm.
awk '
  /^static int arm64_postra_numdynamic_kernel/ { in_kernel = 1 }
  in_kernel &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_ADD_GT) {") {
      copying = 1
    }
  copying { print }
  copying &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_ADD_GE) {") {
      exit
    }
' "$admit_source" >"$postra_addgt_region"

awk '
  /^static int arm64_ir_numdynamic_kernel/ { in_kernel = 1 }
  in_kernel &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_ADD_GT) {") {
      copying = 1
    }
  copying { print }
  copying &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_ADD_GE) {") {
      exit
    }
' "$admit_source" >"$semantic_addgt_region"

for region in "$postra_addgt_region" "$semantic_addgt_region"; do
  test "$(wc -l <"$region" | tr -d ' ')" -eq 7
  for required in \
    '} else if (grammar_profile == ARM64_NUMDYN_ADD_GT) {' \
    'recurrence_op = IR_ADD;' \
    'first_left = stepref;' \
    'first_right = xref;' \
    'preop = IR_LT;' \
    'bodyop = IR_GT;' \
    '} else if (grammar_profile == ARM64_NUMDYN_ADD_GE) {'; do
    grep -F "$required" "$region" >/dev/null || {
      echo "ARM64 ADD_GT kernel tuple changed: $required" >&2
      exit 1
    }
  done
done

# Pin the descending-inclusive ADD semantic and post-RA arms as complete
# scoped tuples, including their boundary with the following SUB_GT arm.
awk '
  /^static int arm64_postra_numdynamic_kernel/ { in_kernel = 1 }
  in_kernel &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_ADD_GE) {") {
      copying = 1
    }
  copying { print }
  copying &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_SUB_GT) {") {
      exit
    }
' "$admit_source" >"$postra_addge_region"

awk '
  /^static int arm64_ir_numdynamic_kernel/ { in_kernel = 1 }
  in_kernel &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_ADD_GE) {") {
      copying = 1
    }
  copying { print }
  copying &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_SUB_GT) {") {
      exit
    }
' "$admit_source" >"$semantic_addge_region"

for region in "$postra_addge_region" "$semantic_addge_region"; do
  test "$(wc -l <"$region" | tr -d ' ')" -eq 7
  for required in \
    '} else if (grammar_profile == ARM64_NUMDYN_ADD_GE) {' \
    'recurrence_op = IR_ADD;' \
    'first_left = stepref;' \
    'first_right = xref;' \
    'preop = IR_LE;' \
    'bodyop = IR_GE;' \
    '} else if (grammar_profile == ARM64_NUMDYN_SUB_GT) {'; do
    grep -F "$required" "$region" >/dev/null || {
      echo "ARM64 ADD_GE kernel tuple changed: $required" >&2
      exit 1
    }
  done
done

# Bind all four ADD profiles to the exact ADDVV A/B/C and comparison A/D
# tuples instead of accepting independent global token matches.
awk '
  /^static unsigned arm64_numacc_grammar_profile/ { in_selector = 1 }
  in_selector &&
    index($0, "if (bc_op(recurrence) == BC_ADDVV") {
      copying = 1
    }
  copying { print }
  copying &&
    index($0, "} else if (bc_op(recurrence) == BC_SUBVV") {
      sub_boundary = 1
    }
  copying && sub_boundary &&
    index($0, "bc_b(recurrence) == 3 && bc_c(recurrence) == 4) {") {
      exit
    }
' "$admit_source" >"$selector_add_region"
test "$(wc -l <"$selector_add_region" | tr -d ' ')" -eq 16
for required in \
  'if (bc_op(recurrence) == BC_ADDVV && bc_a(recurrence) == 3 &&' \
  'bc_b(recurrence) == 3 && bc_c(recurrence) == 4) {' \
  'if (bc_op(compare) == BC_ISGE && bc_a(compare) == 3 &&' \
  'return ARM64_NUMDYN_ADD_LT;' \
  'if (bc_op(compare) == BC_ISGT && bc_a(compare) == 3 &&' \
  'return ARM64_NUMDYN_ADD_LE;' \
  'if (bc_op(compare) == BC_ISGE && bc_a(compare) == 4 &&' \
  'return ARM64_NUMDYN_ADD_GT;' \
  'if (bc_op(compare) == BC_ISGT && bc_a(compare) == 4 &&' \
  'return ARM64_NUMDYN_ADD_GE;' \
  '} else if (bc_op(recurrence) == BC_SUBVV && bc_a(recurrence) == 3 &&'; do
  grep -F "$required" "$selector_add_region" >/dev/null || {
    echo "ARM64 ADD NUM selector tuple changed: $required" >&2
    exit 1
  }
done
test "$(grep -Fc 'bc_d(compare) == 4)' "$selector_add_region")" -eq 2
test "$(grep -Fc 'bc_d(compare) == 3)' "$selector_add_region")" -eq 2
selector_add_sequence=$(tr '\n\t' '  ' <"$selector_add_region" | tr -s ' ')
for required in \
  'if (bc_op(compare) == BC_ISGE && bc_a(compare) == 3 && bc_d(compare) == 4) return ARM64_NUMDYN_ADD_LT;' \
  'if (bc_op(compare) == BC_ISGT && bc_a(compare) == 3 && bc_d(compare) == 4) return ARM64_NUMDYN_ADD_LE;' \
  'if (bc_op(compare) == BC_ISGE && bc_a(compare) == 4 && bc_d(compare) == 3) return ARM64_NUMDYN_ADD_GT;' \
  'if (bc_op(compare) == BC_ISGT && bc_a(compare) == 4 && bc_d(compare) == 3) return ARM64_NUMDYN_ADD_GE;'; do
  case "$selector_add_sequence" in
    *"$required"*) ;;
    *)
      echo "ARM64 ADD NUM selector adjacency changed: $required" >&2
      exit 1
      ;;
  esac
done

# Pin the descending-inclusive semantic and post-RA arms as complete scoped
# tuples, including their boundary with the following MUL_LT arm.
awk '
  /^static int arm64_postra_numdynamic_kernel/ { in_kernel = 1 }
  in_kernel &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_SUB_GE) {") {
      copying = 1
    }
  copying { print }
  copying &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_MUL_LT) {") {
      exit
    }
' "$admit_source" >"$postra_subge_region"

awk '
  /^static int arm64_ir_numdynamic_kernel/ { in_kernel = 1 }
  in_kernel &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_SUB_GE) {") {
      copying = 1
    }
  copying { print }
  copying &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_MUL_LT) {") {
      exit
    }
' "$admit_source" >"$semantic_subge_region"

for region in "$postra_subge_region" "$semantic_subge_region"; do
  test "$(wc -l <"$region" | tr -d ' ')" -eq 7
  for required in \
    '} else if (grammar_profile == ARM64_NUMDYN_SUB_GE) {' \
    'recurrence_op = IR_SUB;' \
    'first_left = xref;' \
    'first_right = stepref;' \
    'preop = IR_LE;' \
    'bodyop = IR_GE;' \
    '} else if (grammar_profile == ARM64_NUMDYN_MUL_LT) {'; do
    grep -F "$required" "$region" >/dev/null || {
      echo "ARM64 SUB_GE kernel tuple changed: $required" >&2
      exit 1
    }
  done
done

# Likewise, bind SUB_GT and SUB_GE to the exact SUBVV A/B/C and comparison
# A/D bytecode tuple rather than accepting independent global token matches.
awk '
  /^static unsigned arm64_numacc_grammar_profile/ { in_selector = 1 }
  in_selector &&
    index($0, "} else if (bc_op(recurrence) == BC_SUBVV") {
      copying = 1
    }
  copying { print }
  copying &&
    index($0, "} else if (bc_op(recurrence) == BC_MULVV") {
      exit
    }
' "$admit_source" >"$selector_sub_region"
test "$(wc -l <"$selector_sub_region" | tr -d ' ')" -eq 9
for required in \
  '} else if (bc_op(recurrence) == BC_SUBVV && bc_a(recurrence) == 3 &&' \
  'bc_b(recurrence) == 3 && bc_c(recurrence) == 4) {' \
  'if (bc_op(compare) == BC_ISGE && bc_a(compare) == 4 &&' \
  'return ARM64_NUMDYN_SUB_GT;' \
  'if (bc_op(compare) == BC_ISGT && bc_a(compare) == 4 &&' \
  'return ARM64_NUMDYN_SUB_GE;' \
  '} else if (bc_op(recurrence) == BC_MULVV && bc_a(recurrence) == 3 &&'; do
  grep -F "$required" "$selector_sub_region" >/dev/null || {
    echo "ARM64 descending NUM selector tuple changed: $required" >&2
    exit 1
  }
done
test "$(grep -Fc 'bc_d(compare) == 3)' "$selector_sub_region")" -eq 2

# MUL_LT and MUL_LE share only the dedicated recurrence authorization. Keep
# each semantic and post-RA tuple independently scoped and adjacent.
awk '
  /^static int arm64_postra_numdynamic_kernel/ { in_kernel = 1 }
  in_kernel &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_MUL_LT) {") {
      copying = 1
    }
  copying { print }
  copying &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_MUL_LE) {") {
      exit
    }
' "$admit_source" >"$postra_mullt_region"

awk '
  /^static int arm64_ir_numdynamic_kernel/ { in_kernel = 1 }
  in_kernel &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_MUL_LT) {") {
      copying = 1
    }
  copying { print }
  copying &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_MUL_LE) {") {
      exit
    }
' "$admit_source" >"$semantic_mullt_region"

for region in "$postra_mullt_region" "$semantic_mullt_region"; do
  test "$(wc -l <"$region" | tr -d ' ')" -eq 7
  for required in \
    '} else if (grammar_profile == ARM64_NUMDYN_MUL_LT) {' \
    'recurrence_op = IR_MUL;' \
    'first_left = stepref;' \
    'first_right = xref;' \
    'preop = IR_GT;' \
    'bodyop = IR_LT;' \
    '} else if (grammar_profile == ARM64_NUMDYN_MUL_LE) {'; do
    grep -F "$required" "$region" >/dev/null || {
      echo "ARM64 MUL_LT kernel tuple changed: $required" >&2
      exit 1
    }
  done
done

awk '
  /^static int arm64_postra_numdynamic_kernel/ { in_kernel = 1 }
  in_kernel &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_MUL_LE) {") {
      copying = 1
    }
  copying { print }
  copying &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_DIV_LT) {") {
      exit
    }
' "$admit_source" >"$postra_mulle_region"

awk '
  /^static int arm64_ir_numdynamic_kernel/ { in_kernel = 1 }
  in_kernel &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_MUL_LE) {") {
      copying = 1
    }
  copying { print }
  copying &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_DIV_LT) {") {
      exit
    }
' "$admit_source" >"$semantic_mulle_region"

for region in "$postra_mulle_region" "$semantic_mulle_region"; do
  test "$(wc -l <"$region" | tr -d ' ')" -eq 7
  for required in \
    '} else if (grammar_profile == ARM64_NUMDYN_MUL_LE) {' \
    'recurrence_op = IR_MUL;' \
    'first_left = stepref;' \
    'first_right = xref;' \
    'preop = IR_GE;' \
    'bodyop = IR_LE;' \
    '} else if (grammar_profile == ARM64_NUMDYN_DIV_LT) {'; do
    grep -F "$required" "$region" >/dev/null || {
      echo "ARM64 MUL_LE kernel tuple changed: $required" >&2
      exit 1
    }
  done
done

awk '
  /^static int arm64_postra_numdynamic_kernel/ { in_kernel = 1 }
  in_kernel &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_DIV_LT) {") {
      copying = 1
    }
  copying { print }
  copying &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_DIV_LE) {") {
      exit
    }
' "$admit_source" >"$postra_divlt_region"

awk '
  /^static int arm64_ir_numdynamic_kernel/ { in_kernel = 1 }
  in_kernel &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_DIV_LT) {") {
      copying = 1
    }
  copying { print }
  copying &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_DIV_LE) {") {
      exit
    }
' "$admit_source" >"$semantic_divlt_region"

for region in "$postra_divlt_region" "$semantic_divlt_region"; do
  test "$(wc -l <"$region" | tr -d ' ')" -eq 7
  for required in \
    '} else if (grammar_profile == ARM64_NUMDYN_DIV_LT) {' \
    'recurrence_op = IR_DIV;' \
    'first_left = xref;' \
    'first_right = stepref;' \
    'preop = IR_GT;' \
    'bodyop = IR_LT;' \
    '} else if (grammar_profile == ARM64_NUMDYN_DIV_LE) {'; do
    grep -F "$required" "$region" >/dev/null || {
      echo "ARM64 DIV_LT kernel tuple changed: $required" >&2
      exit 1
    }
  done
done

awk '
  /^static int arm64_postra_numdynamic_kernel/ { in_kernel = 1 }
  in_kernel &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_DIV_LE) {") {
      copying = 1
    }
  copying { print }
  copying &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_DIV_GT) {") {
      exit
    }
' "$admit_source" >"$postra_divle_region"

awk '
  /^static int arm64_ir_numdynamic_kernel/ { in_kernel = 1 }
  in_kernel &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_DIV_LE) {") {
      copying = 1
    }
  copying { print }
  copying &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_DIV_GT) {") {
      exit
    }
' "$admit_source" >"$semantic_divle_region"

for region in "$postra_divle_region" "$semantic_divle_region"; do
  test "$(wc -l <"$region" | tr -d ' ')" -eq 7
  for required in \
    '} else if (grammar_profile == ARM64_NUMDYN_DIV_LE) {' \
    'recurrence_op = IR_DIV;' \
    'first_left = xref;' \
    'first_right = stepref;' \
    'preop = IR_GE;' \
    'bodyop = IR_LE;' \
    '} else if (grammar_profile == ARM64_NUMDYN_DIV_GT) {'; do
    grep -F "$required" "$region" >/dev/null || {
      echo "ARM64 DIV_LE kernel tuple changed: $required" >&2
      exit 1
    }
  done
done

awk '
  /^static int arm64_postra_numdynamic_kernel/ { in_kernel = 1 }
  in_kernel &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_DIV_GT) {") {
      copying = 1
    }
  copying { print }
  copying &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_DIV_GE) {") {
      exit
    }
' "$admit_source" >"$postra_divgt_region"

awk '
  /^static int arm64_ir_numdynamic_kernel/ { in_kernel = 1 }
  in_kernel &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_DIV_GT) {") {
      copying = 1
    }
  copying { print }
  copying &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_DIV_GE) {") {
      exit
    }
' "$admit_source" >"$semantic_divgt_region"

for region in "$postra_divgt_region" "$semantic_divgt_region"; do
  test "$(wc -l <"$region" | tr -d ' ')" -eq 7
  for required in \
    '} else if (grammar_profile == ARM64_NUMDYN_DIV_GT) {' \
    'recurrence_op = IR_DIV;' \
    'first_left = xref;' \
    'first_right = stepref;' \
    'preop = IR_LT;' \
    'bodyop = IR_GT;' \
    '} else if (grammar_profile == ARM64_NUMDYN_DIV_GE) {'; do
    grep -F "$required" "$region" >/dev/null || {
      echo "ARM64 DIV_GT kernel tuple changed: $required" >&2
      exit 1
    }
  done
done

awk '
  /^static int arm64_postra_numdynamic_kernel/ { in_kernel = 1 }
  in_kernel &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_DIV_GE) {") {
      copying = 1
    }
  copying { print }
  copying && $0 == "  } else {" { exit }
' "$admit_source" >"$postra_divge_region"

awk '
  /^static int arm64_ir_numdynamic_kernel/ { in_kernel = 1 }
  in_kernel &&
    index($0, "} else if (grammar_profile == ARM64_NUMDYN_DIV_GE) {") {
      copying = 1
    }
  copying { print }
  copying && $0 == "  } else {" { exit }
' "$admit_source" >"$semantic_divge_region"

for region in "$postra_divge_region" "$semantic_divge_region"; do
  test "$(wc -l <"$region" | tr -d ' ')" -eq 7
  for required in \
    '} else if (grammar_profile == ARM64_NUMDYN_DIV_GE) {' \
    'recurrence_op = IR_DIV;' \
    'first_left = xref;' \
    'first_right = stepref;' \
    'preop = IR_LE;' \
    'bodyop = IR_GE;' \
    '} else {'; do
    grep -F "$required" "$region" >/dev/null || {
      echo "ARM64 DIV_GE kernel tuple changed: $required" >&2
      exit 1
    }
  done
done

awk '
  /^static unsigned arm64_numacc_grammar_profile/ { in_selector = 1 }
  in_selector &&
    index($0, "} else if (bc_op(recurrence) == BC_MULVV") {
      copying = 1
    }
  copying { print }
  copying &&
    index($0, "} else if (bc_op(recurrence) == BC_DIVVV") { exit }
' "$admit_source" >"$selector_mul_region"
test "$(wc -l <"$selector_mul_region" | tr -d ' ')" -eq 9
for required in \
  '} else if (bc_op(recurrence) == BC_MULVV && bc_a(recurrence) == 3 &&' \
  'bc_b(recurrence) == 3 && bc_c(recurrence) == 4) {' \
  'if (bc_op(compare) == BC_ISGE && bc_a(compare) == 3 &&' \
  'bc_d(compare) == 4)' \
  'return ARM64_NUMDYN_MUL_LT;' \
  'if (bc_op(compare) == BC_ISGT && bc_a(compare) == 3 &&' \
  'return ARM64_NUMDYN_MUL_LE;' \
  '} else if (bc_op(recurrence) == BC_DIVVV && bc_a(recurrence) == 3 &&'; do
  grep -F "$required" "$selector_mul_region" >/dev/null || {
    echo "ARM64 MUL selector tuple changed: $required" >&2
    exit 1
  }
done


awk '
  /^static unsigned arm64_numacc_grammar_profile/ { in_selector = 1 }
  in_selector &&
    index($0, "} else if (bc_op(recurrence) == BC_DIVVV") {
      copying = 1
    }
  copying { print }
  copying && $0 == "  return 0;" { exit }
' "$admit_source" >"$selector_div_region"
test "$(wc -l <"$selector_div_region" | tr -d ' ')" -eq 16
for required in \
  '} else if (bc_op(recurrence) == BC_DIVVV && bc_a(recurrence) == 3 &&' \
  'bc_b(recurrence) == 3 && bc_c(recurrence) == 4) {' \
  'if (bc_op(compare) == BC_ISGE && bc_a(compare) == 3 &&' \
  'bc_d(compare) == 4)' \
  'return ARM64_NUMDYN_DIV_LT;' \
  'if (bc_op(compare) == BC_ISGT && bc_a(compare) == 3 &&' \
  'return ARM64_NUMDYN_DIV_LE;' \
  'if (bc_op(compare) == BC_ISGE && bc_a(compare) == 4 &&' \
  'return ARM64_NUMDYN_DIV_GT;' \
  'if (bc_op(compare) == BC_ISGT && bc_a(compare) == 4 &&' \
  'return ARM64_NUMDYN_DIV_GE;' \
  '}' \
  'return 0;'; do
  grep -F "$required" "$selector_div_region" >/dev/null || {
    echo "ARM64 DIV selector tuple changed: $required" >&2
    exit 1
  }
done
test "$(grep -Fc 'bc_d(compare) == 4)' "$selector_div_region")" -eq 2
test "$(grep -Fc 'bc_d(compare) == 3)' "$selector_div_region")" -eq 2
test "$(grep -Fc 'return ARM64_NUMDYN_' "$selector_div_region")" -eq 4
selector_div_sequence=$(tr '\n\t' '  ' <"$selector_div_region" | tr -s ' ')
case "$selector_div_sequence" in
  *'} else if (bc_op(recurrence) == BC_DIVVV && bc_a(recurrence) == 3 && bc_b(recurrence) == 3 && bc_c(recurrence) == 4) { if (bc_op(compare) == BC_ISGE && bc_a(compare) == 3 && bc_d(compare) == 4) return ARM64_NUMDYN_DIV_LT; if (bc_op(compare) == BC_ISGT && bc_a(compare) == 3 && bc_d(compare) == 4) return ARM64_NUMDYN_DIV_LE; if (bc_op(compare) == BC_ISGE && bc_a(compare) == 4 && bc_d(compare) == 3) return ARM64_NUMDYN_DIV_GT; if (bc_op(compare) == BC_ISGT && bc_a(compare) == 4 && bc_d(compare) == 3) return ARM64_NUMDYN_DIV_GE; } return 0;'*) ;;
  *)
    echo "ARM64 DIV selector adjacency changed" >&2
    exit 1
    ;;
esac
test "$(grep -Fc 'bc_d(compare) == 4)' "$selector_mul_region")" -eq 2
test "$(grep -Fc 'return ARM64_NUMDYN_' "$selector_mul_region")" -eq 2
selector_mul_sequence=$(tr '\n\t' '  ' <"$selector_mul_region" | tr -s ' ')
for required in \
  'if (bc_op(compare) == BC_ISGE && bc_a(compare) == 3 && bc_d(compare) == 4) return ARM64_NUMDYN_MUL_LT;' \
  'if (bc_op(compare) == BC_ISGT && bc_a(compare) == 3 && bc_d(compare) == 4) return ARM64_NUMDYN_MUL_LE;'; do
  case "$selector_mul_sequence" in
    *"$required"*) ;;
    *)
      echo "ARM64 MUL selector adjacency changed: $required" >&2
      exit 1
      ;;
  esac
done

for required in \
  'NUMACC_FIXTURE_ADD_LT = 1u,' \
  'NUMACC_FIXTURE_ADD_LE = 2u,' \
  'NUMACC_FIXTURE_SUB_GT = 3u,' \
  'NUMACC_FIXTURE_SUB_GE = 4u,' \
  'NUMACC_FIXTURE_ADD_GT = 5u,' \
  'NUMACC_FIXTURE_ADD_GE = 6u,' \
  'NUMACC_FIXTURE_MUL_LT = 7u,' \
  'NUMACC_FIXTURE_MUL_LE = 8u,' \
  'NUMACC_FIXTURE_DIV_LT = 9u,' \
  'NUMACC_FIXTURE_DIV_LE = 10u,' \
  'NUMACC_FIXTURE_DIV_GT = 11u,' \
  'NUMACC_FIXTURE_DIV_GE = 12u' \
  'NUMACC_FIXTURE_ARGS_NUM = 1u,' \
  'NUMACC_FIXTURE_ARGS_INT_STEP = 2u,' \
  'NUMACC_FIXTURE_ARGS_INT_LIMIT = 3u' \
  'typedef struct NumaccFixtureProfile {' \
  'BCOp comparison_bc;' \
  'BCOp recurrence_bc;' \
  'IROp recurrence_op;' \
  '{ NUMACC_FIXTURE_ADD_LT, BC_ISGE, 3, 4, BC_ADDVV, IR_ADD,' \
  '{ NUMACC_FIXTURE_ADD_LE, BC_ISGT, 3, 4, BC_ADDVV, IR_ADD,' \
  '{ NUMACC_FIXTURE_SUB_GT, BC_ISGE, 4, 3, BC_SUBVV, IR_SUB,' \
  '{ NUMACC_FIXTURE_SUB_GE, BC_ISGT, 4, 3, BC_SUBVV, IR_SUB,' \
  '{ NUMACC_FIXTURE_ADD_GT, BC_ISGE, 4, 3, BC_ADDVV, IR_ADD,' \
  '{ NUMACC_FIXTURE_ADD_GE, BC_ISGT, 4, 3, BC_ADDVV, IR_ADD,' \
  '{ NUMACC_FIXTURE_MUL_LT, BC_ISGE, 3, 4, BC_MULVV, IR_MUL,' \
  '{ NUMACC_FIXTURE_MUL_LE, BC_ISGT, 3, 4, BC_MULVV, IR_MUL,' \
  '{ NUMACC_FIXTURE_DIV_LT, BC_ISGE, 3, 4, BC_DIVVV, IR_DIV,' \
  '{ NUMACC_FIXTURE_DIV_LE, BC_ISGT, 3, 4, BC_DIVVV, IR_DIV,' \
  '{ NUMACC_FIXTURE_DIV_GT, BC_ISGE, 4, 3, BC_DIVVV, IR_DIV,' \
  '{ NUMACC_FIXTURE_DIV_GE, BC_ISGT, 4, 3, BC_DIVVV, IR_DIV,' \
  'static void select_numacc_fixture(unsigned profile_id)' \
  '} else if (profile_id == NUMACC_FIXTURE_ADD_GT) {' \
  '} else if (profile_id == NUMACC_FIXTURE_ADD_GE) {' \
  '} else if (profile_id == NUMACC_FIXTURE_MUL_LT) {' \
  '} else if (profile_id == NUMACC_FIXTURE_MUL_LE) {' \
  '} else if (profile_id == NUMACC_FIXTURE_DIV_LT) {' \
  '} else if (profile_id == NUMACC_FIXTURE_DIV_LE) {' \
  '} else if (profile_id == NUMACC_FIXTURE_DIV_GT) {' \
  '} else if (profile_id == NUMACC_FIXTURE_DIV_GE) {' \
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
  'AI_R_X = REF_FIRST,' \
  'AI_R_STEP_INT,' \
  'AI_R_STEP_NUM,' \
  'AI_R_X_PRE,' \
  'AI_R_LIMIT,' \
  'AI_R_PRE_GUARD,' \
  'AI_R_LOOP,' \
  'AI_R_XPOLL,' \
  'AI_R_X_BODY,' \
  'AI_R_BODY_GUARD,' \
  'AI_R_X_PHI,' \
  'AI_R_SEMANTIC_END,' \
  'AI_R_NOP = AI_R_SEMANTIC_END,' \
  'AI_R_POSTRA_END' \
  'AIL_R_X = REF_FIRST,' \
  'AIL_R_STEP,' \
  'AIL_R_X_PRE,' \
  'AIL_R_LIMIT_INT,' \
  'AIL_R_LIMIT_NUM,' \
  'AIL_R_PRE_GUARD,' \
  'AIL_R_LOOP,' \
  'AIL_R_XPOLL,' \
  'AIL_R_X_BODY,' \
  'AIL_R_BODY_GUARD,' \
  'AIL_R_X_PHI,' \
  'AIL_R_SEMANTIC_END,' \
  'AIL_R_NOP = AIL_R_SEMANTIC_END,' \
  'AIL_R_POSTRA_END' \
  'static void make_numacc_intstep_trace(jit_State *J)' \
  'setir(AI_R_X, IR_SLOAD, IRT_NUM|IRT_GUARD,' \
  'setir(AI_R_STEP_INT, IR_SLOAD, IRT_INT|IRT_GUARD,' \
  'setir(AI_R_STEP_NUM, IR_CONV, IRT_NUM,' \
  'AI_R_STEP_INT, IRCONV_NUM_INT);' \
  'setir(AI_R_X_PRE, profile->recurrence_op, IRT_NUM|IRT_ISPHI,' \
  'setir(AI_R_LIMIT, IR_SLOAD, IRT_NUM|IRT_GUARD,' \
  'setir(AI_R_PRE_GUARD, profile->precondition_op, IRT_NUM|IRT_GUARD,' \
  'setir(AI_R_LOOP, IR_LOOP, IRT_NIL|IRT_GUARD, 0, 0);' \
  'setir(AI_R_XPOLL, IR_XPOLL, IRT_NIL|IRT_GUARD, 1, 0);' \
  'setir(AI_R_X_BODY, profile->recurrence_op, IRT_NUM|IRT_ISPHI,' \
  'setir(AI_R_BODY_GUARD, profile->body_op, IRT_NUM|IRT_GUARD,' \
  'setir(AI_R_X_PHI, IR_PHI, IRT_NUM, AI_R_X_PRE, AI_R_X_BODY);' \
  'fx.snapmap[2] = SNAP(2, 0, AI_R_X_PRE);' \
  'fx.snapmap[3] = SNAP(5, 0, AI_R_X_PRE);' \
  'fx.snapmap[12] = SNAP(2, 0, AI_R_X_BODY);' \
  'fx.T.nins = AI_R_SEMANTIC_END;' \
  'J->loopref = AI_R_LOOP;' \
  'static void make_numacc_intlimit_trace(jit_State *J)' \
  'AIL_R_X, AIL_R_LIMIT_INT, AIL_R_PRE_GUARD, AIL_R_LOOP,' \
  'setir(AIL_R_X, IR_SLOAD, IRT_NUM|IRT_GUARD,' \
  'setir(AIL_R_STEP, IR_SLOAD, IRT_NUM|IRT_GUARD,' \
  'setir(AIL_R_X_PRE, profile->recurrence_op, IRT_NUM|IRT_ISPHI,' \
  'setir(AIL_R_LIMIT_INT, IR_SLOAD, IRT_INT|IRT_GUARD,' \
  'setir(AIL_R_LIMIT_NUM, IR_CONV, IRT_NUM,' \
  'AIL_R_LIMIT_INT, IRCONV_NUM_INT);' \
  'setir(AIL_R_PRE_GUARD, profile->precondition_op, IRT_NUM|IRT_GUARD,' \
  'AIL_R_LIMIT_NUM, AIL_R_X_PRE);' \
  'setir(AIL_R_X_BODY, profile->recurrence_op, IRT_NUM|IRT_ISPHI,' \
  'setir(AIL_R_BODY_GUARD, profile->body_op, IRT_NUM|IRT_GUARD,' \
  'AIL_R_X_BODY, AIL_R_LIMIT_NUM);' \
  'setir(AIL_R_X_PHI, IR_PHI, IRT_NUM, AIL_R_X_PRE, AIL_R_X_BODY);' \
  'fx.snapmap[2] = SNAP(2, 0, AIL_R_X_PRE);' \
  'fx.snapmap[3] = SNAP(5, 0, AIL_R_X_PRE);' \
  'fx.snapmap[12] = SNAP(2, 0, AIL_R_X_BODY);' \
  'fx.T.nins = AIL_R_SEMANTIC_END;' \
  'J->loopref = AIL_R_LOOP;' \
  'static LJArm64PostRAView make_numacc_intstep_postra_view(jit_State *J)' \
  'fx.ir[AI_R_STEP_INT].r = RID_X1;' \
  'fx.ir[AI_R_STEP_NUM].r = RID_D1;' \
  'fx.ir[AI_R_X_PRE].r = RID_D15;' \
  'fx.ir[AI_R_LIMIT].r = RID_D0;' \
  'setir(AI_R_NOP, IR_NOP, IRT_NIL, 0, 0);' \
  'view.nins = AI_R_POSTRA_END;' \
  'assert(semantic_nins == AI_R_SEMANTIC_END);' \
  'static LJArm64PostRAView make_numacc_intlimit_postra_view(jit_State *J)' \
  'fx.ir[AIL_R_STEP].r = RID_D1;' \
  'fx.ir[AIL_R_X_PRE].r = RID_D15;' \
  'fx.ir[AIL_R_LIMIT_INT].r = RID_X0;' \
  'fx.ir[AIL_R_LIMIT_NUM].r = RID_D0;' \
  'setir(AIL_R_NOP, IR_NOP, IRT_NIL, 0, 0);' \
  'view.nins = AIL_R_POSTRA_END;' \
  'assert(semantic_nins == AIL_R_SEMANTIC_END);' \
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
  'setir(AI_R_NOP, IR_RENAME, IRT_NIL, AI_R_X_PRE, 3);' \
  'setir(AIL_R_NOP, IR_RENAME, IRT_NIL, AIL_R_X_PRE, 3);' \
  'fx.ir[A_R_X].op1 = 4;' \
  'fx.ir[D_R_X].op1 = 2;' \
  'IR_ULT, IR_UGE, IR_ULE, IR_UGT' \
  'fx.ir[A_R_X_PRE].op1 = profile->pre_right;' \
  'fx.ir[A_R_X_PRE].op2 = profile->pre_left;' \
  'fx.ir[A_R_X_PRE].op1 = (IRRef1)profile->pre_right;' \
  'fx.ir[A_R_X_PRE].op2 = (IRRef1)profile->pre_left;' \
  'fx.ir[A_R_X_BODY].op1 = A_R_STEP;' \
  'fx.ir[A_R_X_BODY].op2 = A_R_X_PRE;' \
  'fx.ir[A_R_PRE_GUARD].op1 = A_R_X_PRE;' \
  'fx.ir[A_R_PRE_GUARD].op2 = A_R_LIMIT;' \
  'fx.ir[A_R_BODY_GUARD].op1 = A_R_LIMIT;' \
  'fx.ir[A_R_BODY_GUARD].op2 = A_R_X_BODY;' \
  'fx.T.nk = REF_TRUE-1u;' \
  'fx.T.nk = H_K_HALF;' \
  'REJECT_NUMACC_ADJACENT(IR_CONV, IRT_NUM|IRT_ISPHI,' \
  'fx.ir[AI_R_STEP_NUM].op1 = AI_R_X;' \
  'fx.ir[AI_R_STEP_NUM].op2 = IRCONV_INT_NUM;' \
  'fx.ir[AI_R_STEP_NUM].op2 = IRCONV_NUM_INT|IRCONV_SEXT;' \
  'fx.ir[AI_R_STEP_NUM].op2 = IRCONV_NUM_INT|IRCONV_CHECK;' \
  'fx.ir[AI_R_STEP_NUM].t.irt = IRT_INT;' \
  'fx.ir[AI_R_STEP_NUM].t.irt = IRT_NUM|IRT_GUARD;' \
  'fx.ir[AI_R_STEP_NUM].t.irt = IRT_NUM|IRT_ISPHI;' \
  'setir(AI_R_STEP_NUM, IR_NOP, IRT_NIL, 0, 0);' \
  'duplicate_numacc_intstep_conversion(J, 0);' \
  'duplicate_numacc_intstep_conversion(J, 1);' \
  'relocate_numacc_intstep_conversion();' \
  'fx.ir[AI_R_X_BODY].op2 = AI_R_STEP_INT;' \
  'setir(AI_R_X_PRE, IR_CONV, IRT_NUM, AI_R_X, IRCONV_NUM_INT);' \
  'setir(AI_R_X_BODY, IR_CONV, IRT_NUM, AI_R_X_PRE, IRCONV_NUM_INT);' \
  'REJECT_NUMACC_ADJACENT(IR_SUB, IRT_NUM|IRT_ISPHI,' \
  'REJECT_NUMACC_ADJACENT(IR_MUL, IRT_NUM|IRT_ISPHI,' \
  'REJECT_NUMACC_ADJACENT(IR_DIV, IRT_NUM|IRT_ISPHI,' \
  'fx.snapmap[9] = SNAP(2, SNAP_NORESTORE, A_R_X_PRE);' \
  'for (i = 0; i < 13; i++) {' \
  'bc_publish((const uint32_t *)pc, saved ^ masks[bitno]);' \
  'BCINS_AD(bc_op(saved), profile->comparison_d,' \
  'BCINS_ABC(adjacent, 3, 3, 4));' \
  'BCINS_ABC(profile->recurrence_bc, 3, 4, 3));' \
  'BCINS_ABC(BC_SUBVV, 3, 3, 4));' \
  'BCINS_ABC(BC_MULVV, 3, 3, 4));' \
  'BCINS_ABC(BC_DIVVV, 3, 3, 4));' \
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
  'while x>limit do x=x+step end return x end' \
  'while x>=limit do x=x+step end return x end' \
  'while x>limit do x=x-step end return x end' \
  'while x>=limit do x=x-step end return x end' \
  'numacc_strict_fixture_pt = funcproto(funcV(L->top-1));' \
  'numacc_inclusive_fixture_pt = funcproto(funcV(L->top-1));' \
  'numacc_sub_gt_fixture_pt = funcproto(funcV(L->top-1));' \
  'numacc_sub_ge_fixture_pt = funcproto(funcV(L->top-1));' \
  'numacc_add_gt_fixture_pt = funcproto(funcV(L->top-1));' \
  'numacc_add_ge_fixture_pt = funcproto(funcV(L->top-1));' \
  'numacc_mul_lt_fixture_pt = funcproto(funcV(L->top-1));' \
  'numacc_mul_le_fixture_pt = funcproto(funcV(L->top-1));' \
  'numacc_div_lt_fixture_pt = funcproto(funcV(L->top-1));' \
  'numacc_div_le_fixture_pt = funcproto(funcV(L->top-1));' \
  'numacc_div_gt_fixture_pt = funcproto(funcV(L->top-1));' \
  'numacc_div_ge_fixture_pt = funcproto(funcV(L->top-1));' \
  'assert(numacc_add_gt_fixture_pt->framesize == 5);' \
  'assert(numacc_add_gt_fixture_pt->sizebc == 13);' \
  'assert(numacc_add_gt_fixture_pt->numparams == 3);' \
  'assert(numacc_add_gt_fixture_pt->sizeuv == 0);' \
  'assert(numacc_add_gt_fixture_pt->sizekn == 0);' \
  'assert(numacc_add_gt_fixture_pt->sizekgc == 0);' \
  'assert(numacc_add_gt_fixture_pt->flags2 == PROTO2_CELLOPS);' \
  'BCIns comparison = loadbc(proto_bc(numacc_add_gt_fixture_pt)+3);' \
  'BCIns arithmetic = loadbc(proto_bc(numacc_add_gt_fixture_pt)+8);' \
  'assert(numacc_add_ge_fixture_pt->framesize == 5);' \
  'assert(numacc_add_ge_fixture_pt->sizebc == 13);' \
  'assert(numacc_add_ge_fixture_pt->numparams == 3);' \
  'assert(numacc_add_ge_fixture_pt->sizeuv == 0);' \
  'assert(numacc_add_ge_fixture_pt->sizekn == 0);' \
  'assert(numacc_add_ge_fixture_pt->sizekgc == 0);' \
  'assert(numacc_add_ge_fixture_pt->flags2 == PROTO2_CELLOPS);' \
  'BCIns comparison = loadbc(proto_bc(numacc_add_ge_fixture_pt)+3);' \
  'BCIns arithmetic = loadbc(proto_bc(numacc_add_ge_fixture_pt)+8);' \
  'assert(bc_op(arithmetic) == BC_ADDVV && bc_a(arithmetic) == 3);' \
  'assert(numacc_mul_lt_fixture_pt->framesize == 5);' \
  'assert(numacc_mul_lt_fixture_pt->sizebc == 13);' \
  'assert(numacc_mul_lt_fixture_pt->numparams == 3);' \
  'assert(numacc_mul_lt_fixture_pt->sizeuv == 0);' \
  'assert(numacc_mul_lt_fixture_pt->sizekn == 0);' \
  'assert(numacc_mul_lt_fixture_pt->sizekgc == 0);' \
  'assert(numacc_mul_lt_fixture_pt->flags2 == PROTO2_CELLOPS);' \
  'BCIns comparison = loadbc(proto_bc(numacc_mul_lt_fixture_pt)+3);' \
  'BCIns arithmetic = loadbc(proto_bc(numacc_mul_lt_fixture_pt)+8);' \
  'assert(numacc_mul_le_fixture_pt->framesize == 5);' \
  'assert(numacc_mul_le_fixture_pt->sizebc == 13);' \
  'assert(numacc_mul_le_fixture_pt->numparams == 3);' \
  'assert(numacc_mul_le_fixture_pt->sizeuv == 0);' \
  'assert(numacc_mul_le_fixture_pt->sizekn == 0);' \
  'assert(numacc_mul_le_fixture_pt->sizekgc == 0);' \
  'assert(numacc_mul_le_fixture_pt->flags2 == PROTO2_CELLOPS);' \
  'BCIns comparison = loadbc(proto_bc(numacc_mul_le_fixture_pt)+3);' \
  'BCIns arithmetic = loadbc(proto_bc(numacc_mul_le_fixture_pt)+8);' \
  'assert(bc_op(arithmetic) == BC_MULVV && bc_a(arithmetic) == 3);' \
  'assert(numacc_div_lt_fixture_pt->framesize == 5);' \
  'assert(numacc_div_lt_fixture_pt->sizebc == 13);' \
  'assert(numacc_div_lt_fixture_pt->numparams == 3);' \
  'assert(numacc_div_lt_fixture_pt->sizeuv == 0);' \
  'assert(numacc_div_lt_fixture_pt->sizekn == 0);' \
  'assert(numacc_div_lt_fixture_pt->sizekgc == 0);' \
  'assert(numacc_div_lt_fixture_pt->flags2 == PROTO2_CELLOPS);' \
  'BCIns comparison = loadbc(proto_bc(numacc_div_lt_fixture_pt)+3);' \
  'BCIns arithmetic = loadbc(proto_bc(numacc_div_lt_fixture_pt)+8);' \
  'assert(bc_op(arithmetic) == BC_DIVVV && bc_a(arithmetic) == 3);' \
  'assert(numacc_div_le_fixture_pt->framesize == 5);' \
  'assert(numacc_div_le_fixture_pt->sizebc == 13);' \
  'assert(numacc_div_le_fixture_pt->numparams == 3);' \
  'assert(numacc_div_le_fixture_pt->sizeuv == 0);' \
  'assert(numacc_div_le_fixture_pt->sizekn == 0);' \
  'assert(numacc_div_le_fixture_pt->sizekgc == 0);' \
  'assert(numacc_div_le_fixture_pt->flags2 == PROTO2_CELLOPS);' \
  'BCIns comparison = loadbc(proto_bc(numacc_div_le_fixture_pt)+3);' \
  'BCIns arithmetic = loadbc(proto_bc(numacc_div_le_fixture_pt)+8);' \
  'assert(numacc_div_gt_fixture_pt->framesize == 5);' \
  'assert(numacc_div_gt_fixture_pt->sizebc == 13);' \
  'assert(numacc_div_gt_fixture_pt->numparams == 3);' \
  'assert(numacc_div_gt_fixture_pt->sizeuv == 0);' \
  'assert(numacc_div_gt_fixture_pt->sizekn == 0);' \
  'assert(numacc_div_gt_fixture_pt->sizekgc == 0);' \
  'assert(numacc_div_gt_fixture_pt->flags2 == PROTO2_CELLOPS);' \
  'BCIns comparison = loadbc(proto_bc(numacc_div_gt_fixture_pt)+3);' \
  'BCIns arithmetic = loadbc(proto_bc(numacc_div_gt_fixture_pt)+8);' \
  'assert(numacc_div_ge_fixture_pt->framesize == 5);' \
  'assert(numacc_div_ge_fixture_pt->sizebc == 13);' \
  'assert(numacc_div_ge_fixture_pt->numparams == 3);' \
  'assert(numacc_div_ge_fixture_pt->sizeuv == 0);' \
  'assert(numacc_div_ge_fixture_pt->sizekn == 0);' \
  'assert(numacc_div_ge_fixture_pt->sizekgc == 0);' \
  'assert(numacc_div_ge_fixture_pt->flags2 == PROTO2_CELLOPS);' \
  'BCIns comparison = loadbc(proto_bc(numacc_div_ge_fixture_pt)+3);' \
  'BCIns arithmetic = loadbc(proto_bc(numacc_div_ge_fixture_pt)+8);' \
  'bc_op(loadbc(proto_bc(numacc_fixture_pt)+3)) == BC_ISGE' \
  'bc_op(loadbc(proto_bc(numacc_inclusive_fixture_pt)+3)) == BC_ISGT' \
  'bc_a(comparison) == 4 && bc_d(comparison) == 3' \
  'bc_op(arithmetic) == BC_SUBVV && bc_a(arithmetic) == 3' \
  'static void test_numacc_shape_cross_product(jit_State *J)' \
  'static const unsigned args_kinds[4] = {' \
  'NUMACC_FIXTURE_ARGS_NUM, NUMACC_FIXTURE_ARGS_INT_STEP,' \
  'NUMACC_FIXTURE_ARGS_INT_LIMIT, NUMACC_FIXTURE_ARGS_INT_X' \
  'IROp arithmetic_ops[2];' \
  'arithmetic_ops[0] = profile->recurrence_op;' \
  'arithmetic_ops[1] = profile->recurrence_op == IR_ADD ? IR_SUB : IR_ADD;' \
  'IRRef pre_left = pre_arithmetic == profile->recurrence_op ?' \
  'IRRef pre_right = pre_arithmetic == profile->recurrence_op ?' \
  'static const IROp preops[4] = { IR_GT, IR_GE, IR_LT, IR_LE };' \
  'static const IROp bodyops[4] = { IR_LT, IR_LE, IR_GT, IR_GE };' \
  'int admitted = pre_arithmetic == profile->recurrence_op &&' \
  'body_arithmetic == profile->recurrence_op &&' \
  'preops[pre] == profile->precondition_op &&' \
  'bodyops[body] == profile->body_op &&' \
  '(!intlimit || profile->id == NUMACC_FIXTURE_ADD_LT);' \
  'assert(combinations == 12u*4u*2u*2u*4u*4u);' \
  'assert(combinations == 3072);' \
  'assert(semantic_admissions == 37 && postra_admissions == 37);' \
  'expect_numacc_semantic_result(J, admitted);' \
  'expect_numacc_postra_result(&view, admitted);' \
  'bc_op(saved_compare) == BC_ISGE ? BC_ISGT : BC_ISGE,' \
  'select_numacc_fixture(NUMACC_FIXTURE_ADD_LT);' \
  'select_numacc_fixture(NUMACC_FIXTURE_ADD_LE);' \
  'select_numacc_fixture(NUMACC_FIXTURE_SUB_GT);' \
  'select_numacc_fixture(NUMACC_FIXTURE_SUB_GE);' \
  'select_numacc_fixture(NUMACC_FIXTURE_ADD_GT);' \
  'select_numacc_fixture(NUMACC_FIXTURE_ADD_GE);' \
  'select_numacc_fixture(NUMACC_FIXTURE_MUL_LT);' \
  'select_numacc_fixture(NUMACC_FIXTURE_MUL_LE);' \
  'select_numacc_fixture(NUMACC_FIXTURE_DIV_LT);' \
  'select_numacc_fixture(NUMACC_FIXTURE_DIV_LE);' \
  'select_numacc_fixture(NUMACC_FIXTURE_DIV_GT);' \
  'select_numacc_fixture(NUMACC_FIXTURE_DIV_GE);' \
  'test_numacc_shape_cross_product(J);' \
  'L->top -= 17;' \
  'test_numacc_positive_and_negative(J);' \
  'test_numacc_postra_layout(J);' \
  'test_numacc_intstep_positive_and_negative(J);' \
  'test_numacc_intstep_postra_layout(J);' \
  'test_numacc_intlimit_positive_and_negative(J);' \
  'test_numacc_intlimit_postra_layout(J);' \
  'test_numacc_intx_positive_and_negative(J);' \
  'test_numacc_intx_postra_layout(J);'; do
  grep -F "$required" "$root/tests/t-arm64-jit-ir-admission.c" >/dev/null || {
    echo "ARM64 dynamic-accumulator NUM mutation coverage changed: $required" >&2
    exit 1
  }
done
for region in "$numacc_intlimit_semantic_test_region" \
	      "$numacc_intlimit_postra_test_region"; do
  for required in \
    'fx.ir[AIL_R_LIMIT_NUM].op1 = AIL_R_X;' \
    'fx.ir[AIL_R_LIMIT_NUM].op1 = REF_TRUE-1u;' \
    'fx.ir[AIL_R_LIMIT_NUM].op2 = IRCONV_INT_NUM;' \
    'fx.ir[AIL_R_LIMIT_NUM].op2 = IRCONV_NUM_INT|IRCONV_SEXT;' \
    'fx.ir[AIL_R_LIMIT_NUM].op2 = IRCONV_NUM_INT|IRCONV_CHECK;' \
    'fx.ir[AIL_R_LIMIT_NUM].t.irt = IRT_INT;' \
    'fx.ir[AIL_R_LIMIT_NUM].t.irt = IRT_NUM|IRT_GUARD;' \
    'fx.ir[AIL_R_LIMIT_NUM].t.irt = IRT_NUM|IRT_ISPHI;' \
    'setir(AIL_R_LIMIT_NUM, IR_NOP, IRT_NIL, 0, 0);' \
    'relocate_numacc_intlimit_conversion();' \
    'fx.ir[AIL_R_PRE_GUARD].op1 = AIL_R_LIMIT_INT;' \
    'fx.ir[AIL_R_BODY_GUARD].op2 = AIL_R_LIMIT_INT;' \
    'setir(AIL_R_X_PRE, IR_CONV, IRT_NUM, AIL_R_X, IRCONV_NUM_INT);' \
    'setir(AIL_R_X_BODY, IR_CONV, IRT_NUM, AIL_R_X_PRE, IRCONV_NUM_INT);'; do
    grep -F "$required" "$region" >/dev/null || {
      echo "ARM64 INT-limit mutation coverage changed in $region: " \
	"$required" >&2
      exit 1
    }
  done
done
grep -F 'duplicate_numacc_intlimit_conversion(J, 0);' \
  "$numacc_intlimit_semantic_test_region" >/dev/null
grep -F 'duplicate_numacc_intlimit_conversion(J, 1);' \
  "$numacc_intlimit_postra_test_region" >/dev/null

for required in \
  'setir(AX_R_X_INT, IR_SLOAD, IRT_INT|IRT_GUARD,' \
  'setir(AX_R_STEP, IR_SLOAD, IRT_NUM|IRT_GUARD,' \
  'setir(AX_R_X_NUM, IR_CONV, IRT_NUM,' \
  'AX_R_X_INT, IRCONV_NUM_INT);' \
  'setir(AX_R_X_PRE, profile->recurrence_op, IRT_NUM|IRT_ISPHI,' \
  'AX_R_X_NUM, AX_R_STEP);' \
  'setir(AX_R_X_CHECK, IR_CONV, IRT_INT|IRT_GUARD,' \
  'AX_R_X_PRE, IRCONV_INT_NUM|IRCONV_CHECK);' \
  'setir(AX_R_X_BODY, profile->recurrence_op, IRT_NUM|IRT_ISPHI,' \
  'setir(AX_R_X_PHI, IR_PHI, IRT_NUM, AX_R_X_PRE, AX_R_X_BODY);' \
  'fx.snapmap[2] = SNAP(2, 0, AX_R_X_PRE);' \
  'fx.snapmap[3] = SNAP(5, 0, AX_R_X_PRE);' \
  'fx.snapmap[12] = SNAP(2, 0, AX_R_X_BODY);' \
  'fx.T.nins = AX_R_SEMANTIC_END;' \
  'J->loopref = AX_R_LOOP;'; do
  grep -F "$required" "$numacc_intx_region" >/dev/null || {
    echo "ARM64 INT-X recorder fixture changed: $required" >&2
    exit 1
  }
done
for required in \
  'fx.ir[AX_R_X_NUM].op1 = AX_R_STEP;' \
  'fx.ir[AX_R_X_NUM].op2 = IRCONV_INT_NUM;' \
  'fx.ir[AX_R_X_NUM].t.irt = IRT_NUM|IRT_GUARD;' \
  'fx.ir[AX_R_X_CHECK].op1 = AX_R_X_NUM;' \
  'fx.ir[AX_R_X_CHECK].op2 = IRCONV_INT_NUM;' \
  'fx.ir[AX_R_X_CHECK].op2 = IRCONV_NUM_INT|IRCONV_CHECK;' \
  'fx.ir[AX_R_X_CHECK].t.irt = IRT_INT;' \
  'fx.ir[AX_R_X_PRE].op1 = AX_R_X_INT;' \
  'fx.ir[AX_R_X_BODY].op1 = AX_R_X_CHECK;'; do
  grep -F "$required" "$numacc_intx_semantic_test_region" >/dev/null || {
    echo "ARM64 semantic INT-X mutation coverage changed: $required" >&2
    exit 1
  }
done
for required in \
  'fx.ir[AX_R_X_INT].r = RID_X1;' \
  'fx.ir[AX_R_STEP].r = RID_D1;' \
  'fx.ir[AX_R_X_NUM].r = RID_D2;' \
  'fx.ir[AX_R_X_PRE].r = RID_D15;' \
  'fx.ir[AX_R_LIMIT].r = RID_D0;' \
  'fx.ir[AX_R_X_CHECK].r = RID_X28;' \
  'fx.ir[AX_R_X_BODY].r = RID_D15;' \
  'fx.ir[AX_R_X_PHI].r = RID_D15;' \
  'fx.ir[AX_R_PRE_GUARD].r = RID_INIT;' \
  'fx.ir[AX_R_LOOP].r = RID_INIT;' \
  'fx.ir[AX_R_XPOLL].r = RID_INIT;' \
  'fx.ir[AX_R_BODY_GUARD].r = RID_INIT;' \
  'fx.ir[AX_R_X_INT].r = RID_D2;' \
  'fx.ir[AX_R_X_CHECK].r = RID_D2;' \
  'fx.ir[AX_R_X_CHECK].r = RID_NONE;' \
  'fx.ir[AX_R_LIMIT].r = RID_D1;' \
  'fx.ir[AX_R_X_NUM].r = RID_D1;' \
  'view.nins = AX_R_SEMANTIC_END;'; do
  grep -F "$required" "$root/tests/t-arm64-jit-ir-admission.c" >/dev/null || {
    echo "ARM64 post-RA INT-X register coverage changed: $required" >&2
    exit 1
  }
done
test "$(grep -Fc '{ NUMACC_FIXTURE_ADD_GT, BC_ISGE, 4, 3, BC_ADDVV, IR_ADD,' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 1
test "$(grep -Fc 'A_R_STEP, A_R_X, IR_LT, IR_GT }' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 1
test "$(grep -Fc '{ NUMACC_FIXTURE_ADD_GE, BC_ISGT, 4, 3, BC_ADDVV, IR_ADD,' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 1
test "$(grep -Fc 'A_R_STEP, A_R_X, IR_LE, IR_GE }' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 1
test "$(grep -Fc 'while x>limit do x=x+step end return x end' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 1
test "$(grep -Fc 'numacc_add_gt_fixture_pt = funcproto(funcV(L->top-1));' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 1
test "$(grep -Fc 'while x>=limit do x=x+step end return x end' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 1
test "$(grep -Fc 'numacc_add_ge_fixture_pt = funcproto(funcV(L->top-1));' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 1
test "$(grep -Fc '{ NUMACC_FIXTURE_MUL_LT, BC_ISGE, 3, 4, BC_MULVV, IR_MUL,' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 1
test "$(grep -Fc '{ NUMACC_FIXTURE_MUL_LE, BC_ISGT, 3, 4, BC_MULVV, IR_MUL,' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 1
test "$(grep -Fc '{ NUMACC_FIXTURE_DIV_LT, BC_ISGE, 3, 4, BC_DIVVV, IR_DIV,' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 1
test "$(grep -Fc '{ NUMACC_FIXTURE_DIV_LE, BC_ISGT, 3, 4, BC_DIVVV, IR_DIV,' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 1
test "$(grep -Fc '{ NUMACC_FIXTURE_DIV_GT, BC_ISGE, 4, 3, BC_DIVVV, IR_DIV,' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 1
test "$(grep -Fc '{ NUMACC_FIXTURE_DIV_GE, BC_ISGT, 4, 3, BC_DIVVV, IR_DIV,' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 1
numacc_profiles_sequence=$(awk '
  /^static const NumaccFixtureProfile numacc_fixture_profiles\[\]/ {
    copying = 1
  }
  copying { print }
  copying && /^};/ { exit }
' "$root/tests/t-arm64-jit-ir-admission.c" | tr '\n\t' '  ' | tr -s ' ')
case "$numacc_profiles_sequence" in
  *'{ NUMACC_FIXTURE_MUL_LT, BC_ISGE, 3, 4, BC_MULVV, IR_MUL, A_R_STEP, A_R_X, IR_GT, IR_LT }'*) ;;
  *)
    echo "ARM64 MUL_LT synthetic profile tuple changed" >&2
    exit 1
    ;;
esac
case "$numacc_profiles_sequence" in
  *'{ NUMACC_FIXTURE_MUL_LE, BC_ISGT, 3, 4, BC_MULVV, IR_MUL, A_R_STEP, A_R_X, IR_GE, IR_LE }'*) ;;
  *)
    echo "ARM64 MUL_LE synthetic profile tuple changed" >&2
    exit 1
    ;;
esac
case "$numacc_profiles_sequence" in
  *'{ NUMACC_FIXTURE_DIV_LT, BC_ISGE, 3, 4, BC_DIVVV, IR_DIV, A_R_X, A_R_STEP, IR_GT, IR_LT }'*) ;;
  *)
    echo "ARM64 DIV_LT synthetic profile tuple changed" >&2
    exit 1
    ;;
esac
case "$numacc_profiles_sequence" in
  *'{ NUMACC_FIXTURE_DIV_LE, BC_ISGT, 3, 4, BC_DIVVV, IR_DIV, A_R_X, A_R_STEP, IR_GE, IR_LE }'*) ;;
  *)
    echo "ARM64 DIV_LE synthetic profile tuple changed" >&2
    exit 1
    ;;
esac
case "$numacc_profiles_sequence" in
  *'{ NUMACC_FIXTURE_DIV_GT, BC_ISGE, 4, 3, BC_DIVVV, IR_DIV, A_R_X, A_R_STEP, IR_LT, IR_GT }'*) ;;
  *)
    echo "ARM64 DIV_GT synthetic profile tuple changed" >&2
    exit 1
    ;;
esac
case "$numacc_profiles_sequence" in
  *'{ NUMACC_FIXTURE_DIV_GE, BC_ISGT, 4, 3, BC_DIVVV, IR_DIV, A_R_X, A_R_STEP, IR_LE, IR_GE }'*) ;;
  *)
    echo "ARM64 DIV_GE synthetic profile tuple changed" >&2
    exit 1
    ;;
esac
test "$(grep -Fc 'while x<limit do x=x*factor end return x end' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 1
test "$(grep -Fc 'numacc_mul_lt_fixture_pt = funcproto(funcV(L->top-1));' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 1
test "$(grep -Fc 'while x<=limit do x=x*factor end return x end' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 1
test "$(grep -Fc 'numacc_mul_le_fixture_pt = funcproto(funcV(L->top-1));' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 1
awk '
  index($0, "while x<=limit do x=x*factor end return x end") {
    copying = 1
  }
  copying && /^  assert\(luaL_loadstring\(L,$/ { exit }
  copying { print }
' "$root/tests/t-arm64-jit-ir-admission.c" >"$numacc_mulle_fixture_region"
test -s "$numacc_mulle_fixture_region"
for required in \
  'while x<=limit do x=x*factor end return x end' \
  'numacc_mul_le_fixture_pt = funcproto(funcV(L->top-1));' \
  'assert(numacc_mul_le_fixture_loop_pc ==' \
  'proto_bc(numacc_mul_le_fixture_pt)+5);' \
  'assert(bc_op(comparison) == BC_ISGT);' \
  'assert(bc_a(comparison) == 3 && bc_d(comparison) == 4);' \
  'assert(bc_op(arithmetic) == BC_MULVV && bc_a(arithmetic) == 3);' \
  'assert(bc_b(arithmetic) == 3 && bc_c(arithmetic) == 4);'; do
  grep -F "$required" "$numacc_mulle_fixture_region" >/dev/null || {
    echo "ARM64 MUL_LE synthetic source certificate changed: $required" >&2
    exit 1
  }
done
test "$(grep -Fc 'BC_ISGT' "$numacc_mulle_fixture_region")" -eq 1
test "$(grep -Fc 'BC_MULVV' "$numacc_mulle_fixture_region")" -eq 1
for adjacent in BC_ISGE BC_ADDVV BC_SUBVV BC_DIVVV; do
  if grep -F "$adjacent" "$numacc_mulle_fixture_region" >/dev/null; then
    echo "ARM64 MUL_LE synthetic source admitted adjacent $adjacent" >&2
    exit 1
  fi
done
test "$(grep -Fc 'while x<limit do x=x/divisor end return x end' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 1
test "$(grep -Fc 'numacc_div_lt_fixture_pt = funcproto(funcV(L->top-1));' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 1
awk '
  index($0, "while x<limit do x=x/divisor end return x end") {
    copying = 1
  }
  copying && /^  assert\(luaL_loadstring\(L,$/ { exit }
  copying { print }
' "$root/tests/t-arm64-jit-ir-admission.c" >"$numacc_divlt_fixture_region"
test -s "$numacc_divlt_fixture_region"
for required in \
  'while x<limit do x=x/divisor end return x end' \
  'numacc_div_lt_fixture_pt = funcproto(funcV(L->top-1));' \
  'assert(numacc_div_lt_fixture_loop_pc ==' \
  'proto_bc(numacc_div_lt_fixture_pt)+5);' \
  'assert(bc_op(comparison) == BC_ISGE);' \
  'assert(bc_a(comparison) == 3 && bc_d(comparison) == 4);' \
  'assert(bc_op(arithmetic) == BC_DIVVV && bc_a(arithmetic) == 3);' \
  'assert(bc_b(arithmetic) == 3 && bc_c(arithmetic) == 4);'; do
  grep -F "$required" "$numacc_divlt_fixture_region" >/dev/null || {
    echo "ARM64 DIV_LT synthetic source certificate changed: $required" >&2
    exit 1
  }
done
test "$(grep -Fc 'BC_ISGE' "$numacc_divlt_fixture_region")" -eq 1
test "$(grep -Fc 'BC_DIVVV' "$numacc_divlt_fixture_region")" -eq 1
for adjacent in BC_ISGT BC_ADDVV BC_SUBVV BC_MULVV; do
  if grep -F "$adjacent" "$numacc_divlt_fixture_region" >/dev/null; then
    echo "ARM64 DIV_LT synthetic source admitted adjacent $adjacent" >&2
    exit 1
  fi
done

test "$(grep -Fc 'while x<=limit do x=x/divisor end return x end' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 1
test "$(grep -Fc 'numacc_div_le_fixture_pt = funcproto(funcV(L->top-1));' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 1
awk '
  index($0, "while x<=limit do x=x/divisor end return x end") {
    copying = 1
  }
  copying && /^  assert\(luaL_loadstring\(L,$/ { exit }
  copying { print }
' "$root/tests/t-arm64-jit-ir-admission.c" >"$numacc_divle_fixture_region"
test -s "$numacc_divle_fixture_region"
for required in \
  'while x<=limit do x=x/divisor end return x end' \
  'numacc_div_le_fixture_pt = funcproto(funcV(L->top-1));' \
  'assert(numacc_div_le_fixture_loop_pc ==' \
  'proto_bc(numacc_div_le_fixture_pt)+5);' \
  'assert(bc_op(comparison) == BC_ISGT);' \
  'assert(bc_a(comparison) == 3 && bc_d(comparison) == 4);' \
  'assert(bc_op(arithmetic) == BC_DIVVV && bc_a(arithmetic) == 3);' \
  'assert(bc_b(arithmetic) == 3 && bc_c(arithmetic) == 4);'; do
  grep -F "$required" "$numacc_divle_fixture_region" >/dev/null || {
    echo "ARM64 DIV_LE synthetic source certificate changed: $required" >&2
    exit 1
  }
done
test "$(grep -Fc 'BC_ISGT' "$numacc_divle_fixture_region")" -eq 1
test "$(grep -Fc 'BC_DIVVV' "$numacc_divle_fixture_region")" -eq 1
for adjacent in BC_ISGE BC_ADDVV BC_SUBVV BC_MULVV; do
  if grep -F "$adjacent" "$numacc_divle_fixture_region" >/dev/null; then
    echo "ARM64 DIV_LE synthetic source admitted adjacent $adjacent" >&2
    exit 1
  fi
done

test "$(grep -Fc 'while x>limit do x=x/divisor end return x end' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 1
test "$(grep -Fc 'numacc_div_gt_fixture_pt = funcproto(funcV(L->top-1));' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 1
awk '
  index($0, "while x>limit do x=x/divisor end return x end") {
    copying = 1
  }
  copying && /^  assert\(luaL_loadstring\(L,$/ { exit }
  copying { print }
' "$root/tests/t-arm64-jit-ir-admission.c" >"$numacc_divgt_fixture_region"
test -s "$numacc_divgt_fixture_region"
for required in \
  'while x>limit do x=x/divisor end return x end' \
  'numacc_div_gt_fixture_pt = funcproto(funcV(L->top-1));' \
  'assert(numacc_div_gt_fixture_loop_pc ==' \
  'proto_bc(numacc_div_gt_fixture_pt)+5);' \
  'assert(bc_op(comparison) == BC_ISGE);' \
  'assert(bc_a(comparison) == 4 && bc_d(comparison) == 3);' \
  'assert(bc_op(arithmetic) == BC_DIVVV && bc_a(arithmetic) == 3);' \
  'assert(bc_b(arithmetic) == 3 && bc_c(arithmetic) == 4);'; do
  grep -F "$required" "$numacc_divgt_fixture_region" >/dev/null || {
    echo "ARM64 DIV_GT synthetic source certificate changed: $required" >&2
    exit 1
  }
done
test "$(grep -Fc 'BC_ISGE' "$numacc_divgt_fixture_region")" -eq 1
test "$(grep -Fc 'BC_DIVVV' "$numacc_divgt_fixture_region")" -eq 1
for adjacent in BC_ISGT BC_ADDVV BC_SUBVV BC_MULVV; do
  if grep -F "$adjacent" "$numacc_divgt_fixture_region" >/dev/null; then
    echo "ARM64 DIV_GT synthetic source admitted adjacent $adjacent" >&2
    exit 1
  fi
done

test "$(grep -Fc 'while x>=limit do x=x/divisor end return x end' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 1
test "$(grep -Fc 'numacc_div_ge_fixture_pt = funcproto(funcV(L->top-1));' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 1
awk '
  index($0, "while x>=limit do x=x/divisor end return x end") {
    copying = 1
  }
  copying { print }
  copying && /^  J = L2J\(L\);/ { exit }
' "$root/tests/t-arm64-jit-ir-admission.c" >"$numacc_divge_fixture_region"
test -s "$numacc_divge_fixture_region"
for required in \
  'while x>=limit do x=x/divisor end return x end' \
  'numacc_div_ge_fixture_pt = funcproto(funcV(L->top-1));' \
  'assert(numacc_div_ge_fixture_loop_pc ==' \
  'proto_bc(numacc_div_ge_fixture_pt)+5);' \
  'assert(bc_op(comparison) == BC_ISGT);' \
  'assert(bc_a(comparison) == 4 && bc_d(comparison) == 3);' \
  'assert(bc_op(arithmetic) == BC_DIVVV && bc_a(arithmetic) == 3);' \
  'assert(bc_b(arithmetic) == 3 && bc_c(arithmetic) == 4);'; do
  grep -F "$required" "$numacc_divge_fixture_region" >/dev/null || {
    echo "ARM64 DIV_GE synthetic source certificate changed: $required" >&2
    exit 1
  }
done
test "$(grep -Fc 'BC_ISGT' "$numacc_divge_fixture_region")" -eq 1
test "$(grep -Fc 'BC_DIVVV' "$numacc_divge_fixture_region")" -eq 1
for adjacent in BC_ISGE BC_ADDVV BC_SUBVV BC_MULVV; do
  if grep -F "$adjacent" "$numacc_divge_fixture_region" >/dev/null; then
    echo "ARM64 DIV_GE synthetic source admitted adjacent $adjacent" >&2
    exit 1
  fi
done
if grep -F 'pre == p' "$root/tests/t-arm64-jit-ir-admission.c" >/dev/null ||
   grep -F 'body == p' "$root/tests/t-arm64-jit-ir-admission.c" >/dev/null ||
   grep -F 'expected_arithmetic' "$root/tests/t-arm64-jit-ir-admission.c" >/dev/null; then
  echo "ARM64 NUM cross-product coherence still depends on profile indexing" >&2
  exit 1
fi
test "$(grep -Fc 'test_numacc_positive_and_negative(J);' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 12
test "$(grep -Fc 'test_numacc_postra_layout(J);' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 12
test "$(grep -Fc 'test_numacc_intstep_positive_and_negative(J);' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 12
test "$(grep -Fc 'test_numacc_intstep_postra_layout(J);' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 12
test "$(grep -Fc 'test_numacc_intlimit_positive_and_negative(J);' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 1
test "$(grep -Fc 'test_numacc_intlimit_postra_layout(J);' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 1
test "$(grep -Fc 'test_numacc_intx_positive_and_negative(J);' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 12
test "$(grep -Fc 'test_numacc_intx_postra_layout(J);' \
  "$root/tests/t-arm64-jit-ir-admission.c")" -eq 12

# Bind each profile selection to the exhaustive NUM, INT-step and INT-X suites
# inside main. Bare selector tokens and global call counts must not let one
# profile run twice while another profile's proof becomes dead.
awk '
  /^int main\(void\)/ { in_main = 1 }
  in_main &&
    /^  select_numacc_fixture\(NUMACC_FIXTURE_ADD_LT\);/ { copying = 1 }
  copying { print }
  copying && /^  test_numacc_shape_cross_product\(J\);/ { exit }
' "$root/tests/t-arm64-jit-ir-admission.c" >"$numacc_main_region"
test -s "$numacc_main_region"
numacc_main_sequence=$(tr '\n\t' '  ' <"$numacc_main_region" | tr -s ' ')
for profile in ADD_LT ADD_LE SUB_GT SUB_GE ADD_GT ADD_GE MUL_LT MUL_LE DIV_LT DIV_LE DIV_GT DIV_GE; do
  required="select_numacc_fixture(NUMACC_FIXTURE_$profile); test_numacc_positive_and_negative(J); test_numacc_postra_layout(J);"
  case "$numacc_main_sequence" in
    *"$required"*) ;;
    *)
      echo "ARM64 NUM fixture lost exact $profile suite sequence" >&2
      exit 1
      ;;
  esac
  required="select_numacc_fixture(NUMACC_FIXTURE_$profile); test_numacc_intstep_positive_and_negative(J); test_numacc_intstep_postra_layout(J);"
  case "$numacc_main_sequence" in
    *"$required"*) ;;
    *)
      echo "ARM64 INT-step NUM fixture lost exact $profile suite sequence" >&2
      exit 1
      ;;
  esac
  required="select_numacc_fixture(NUMACC_FIXTURE_$profile); test_numacc_intx_positive_and_negative(J); test_numacc_intx_postra_layout(J);"
  case "$numacc_main_sequence" in
    *"$required"*) ;;
    *)
      echo "ARM64 INT-X NUM fixture lost exact $profile suite sequence" >&2
      exit 1
      ;;
  esac
  expected_selects=3
  test "$profile" = ADD_LT && expected_selects=4
  test "$(grep -Fc \
    "select_numacc_fixture(NUMACC_FIXTURE_$profile);" \
    "$numacc_main_region")" -eq "$expected_selects"
done
required='select_numacc_fixture(NUMACC_FIXTURE_ADD_LT); test_numacc_intlimit_positive_and_negative(J); test_numacc_intlimit_postra_layout(J); test_numacc_shape_cross_product(J);'
case "$numacc_main_sequence" in
  *"$required"*) ;;
  *)
    echo "ARM64 INT-limit fixture lost exact ADD_LT suite sequence" >&2
    exit 1
    ;;
esac
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
  'case IR_SUB:' \
  'case IR_MUL:' \
  'case IR_DIV:' \
  'if (!allow_num_mul || irt_type(ins.t) != IRT_NUM)' \
  'if (!allow_num_div || irt_type(ins.t) != IRT_NUM)' \
  '!arm64_postra_num_value(ins, rootop, maxslots, allow_num_sub,' \
  'allow_num_mul, allow_num_div)' \
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
  '} else if (constant_profile != ARM64_IR_KPROFILE_INT ||' \
  'suffix_is_nop || nrename != 1u || spadjust != 0 ||' \
  'highest_end != 0 ||' \
  '!arm64_postra_numadd_shape(view, semantic_nins)) {' \
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
  'case IR_CONV:' \
  'case IR_LT: case IR_GE: case IR_LE: case IR_GT:' \
  'case IR_EQ: case IR_NE:' \
  'case IR_ADDOV: case IR_SUBOV: case IR_MULOV:' \
  'case IR_ADD:' \
  'case IR_SUB:' \
  'case IR_MUL:' \
  'case IR_DIV:' \
  'case IR_PHI:' \
  'case IR_LOOP:' \
  'case IR_XPOLL:' \
  'case IR_USE:' \
  'case IR_CALLN: case IR_CALLA: case IR_CALLL: case IR_CALLS:' \
  'case IR_CALLXS:'; do
  grep -F "$cases" "$semantic_region" >/dev/null || {
    echo "ARM64 IR classifier inventory changed: $cases" >&2
    exit 1
  }
done

awk '/case IR_CALLN:/ { copying = 1 }
     copying && /case IR_USE:/ { exit }
     copying { print }' "$semantic_region" >"$call_region"
test "$(grep -c 'LJ_ARM64_IR_REJECT_CALL' "$call_region")" -eq 2
if grep -E 'break;|return 1|IRCALL_[A-Za-z0-9_]+[[:space:]]*:' \
     "$call_region" >/dev/null; then
  echo "ARM64 CALL helper allowlist is no longer empty" >&2
  exit 1
fi

for forbidden in IR_KGC IR_KPTR IR_KKPTR IR_KNULL IR_KINT64 IR_KSLOT \
  IR_NOP IR_ULT IR_UGE IR_ULE IR_UGT \
  IR_NEG IR_MOD IR_POW \
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
  'arm64_ir_num_value_op((IROp)ir->o, allow_sub, allow_mul, allow_div)' \
  'startop != BC_LOOP || ir->t.irt != (IRT_NUM|IRT_ISPHI)' \
  '!arm64_ir_num_binary(T, ir, ref, allow_num_sub, allow_num_mul,' \
  'allow_num_div)' \
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
  'setir(R_SUM1, IR_DIV, IRT_NUM, R_A, R_B);' \
  'expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_DIV);' \
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

# Fixed-size CNEW must use the same publish-safe helper as the x64 backend.
# The upstream raw allocator leaves a lockless arena allocation in CONSTRUCT
# after the generated stores initialize its header.
awk '/^static void asm_cnew/ { copying = 1 }
     copying { print }
     copying && /^\/\* -- Write barriers/ { exit }' \
  "$root/src/lj_asm_arm64.h" >"$arm64_cnew_region"
for required in \
  'lj_ctype_info_predefined(cts, id, &info, &sz, NULL, NULL)' \
  'lj_ctype_info_snapshot(cts, id, &info, &sz, NULL, NULL)' \
  'IRCALL_lj_cdata_new_forjit' \
  'args[1] = ir->op1;' \
  'args[2] = ASMREF_TMP1;'; do
  grep -F "$required" "$arm64_cnew_region" >/dev/null || {
    echo "ARM64 publish-safe CNEW path changed: $required" >&2
    exit 1
  }
done
if grep -F 'IRCALL_lj_mem_newgco' "$arm64_cnew_region" >/dev/null; then
  echo "ARM64 CNEW regressed to the unpublished raw allocator" >&2
  exit 1
fi

# arm64e/BTI compilation catches pointer-auth and branch-tracking target drift
# without executing any admitted trace. Verify the intended ABI feature macros
# first, so unsupported or misspelled flags cannot silently weaken this audit.
# shellcheck disable=SC2086 # pauth_xcflags intentionally expands to arguments.
"$cc" -arch arm64e -mbranch-protection=bti \
  -mmacosx-version-min="$minver" $pauth_xcflags -I"$root/src" \
  -x c -dM -E -include lj_arch.h /dev/null >"$pauth_macros"
for required in \
  '#define LJ_TARGET_ARM64 1' \
  '#define LJ_ABI_PAUTH 1' \
  '#define LJ_ABI_BRANCH_TRACK 1'; do
  grep -F "$required" "$pauth_macros" >/dev/null || {
    echo "ARM64e/BTI audit macro missing: $required" >&2
    exit 1
  }
done

# shellcheck disable=SC2086 # pauth_xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O0 -Wall -Wextra -Werror -arch arm64e \
  -mbranch-protection=bti -mmacosx-version-min="$minver" \
  $pauth_xcflags -I"$root/src" \
  -c "$asm_source" -o "$audit_object"

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  "$root/tests/t-arm64-jit-ir-admission.c" "$archive" -lm -pthread \
  -o "$fixture"
"$fixture"

echo "arm64_jit_ir_admission_contract OK: exact variable-step INT FORL plus integer, mixed-NUM, fixed-half, dynamic-step and ADD_LT/ADD_LE/ADD_GT/ADD_GE/SUB_GT/SUB_GE/MUL_LT/MUL_LE/DIV_LT/DIV_LE/DIV_GT/DIV_GE dynamic-accumulator NUM/INT-step/INT-X plus ADD_LT INT-limit LOOP grammars, 3072 LOOP combinations with 37 semantic/post-RA admissions, bounded integer spills, and exact GPR/FPR layouts verified"
