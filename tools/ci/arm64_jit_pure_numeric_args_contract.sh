#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_pure_numeric_args_contract SKIP: requires native macOS arm64"
  exit 0
fi

jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
cc=${CC:-clang}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS -DLUAJIT_MCODE_TEST'
pauth_xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR"
archive=$root/src/libluajit.a
fixture_source=$root/tests/t-arm64-jit-pure-numeric-args.c
backend_source=$root/src/lj_asm_arm64.h
target_source=$root/src/lj_target_arm64.h
lock_dir=$root/src/.lj-test-run.lock
lock_held=0
restore_needed=0
tmpdir=

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if test "$restore_needed" = 1; then
    env MACOSX_DEPLOYMENT_TARGET="$minver" \
      make -C "$root/src" clean TARGET_FLAGS='-arch arm64' \
        XCFLAGS="$xcflags" >/dev/null 2>&1 || status=1
    env MACOSX_DEPLOYMENT_TARGET="$minver" \
      make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
        XCFLAGS="$xcflags" >/dev/null 2>&1 || status=1
  fi
  if test "$lock_held" = 1; then
    rm -f "$lock_dir/owner"
    rmdir "$lock_dir" 2>/dev/null || true
  fi
  if test -n "$tmpdir"; then
    rm -rf "$tmpdir"
  fi
  exit "$status"
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

acquire_lock() {
  if test "${LJ_TEST_DISABLE_RUN_LOCK:-}" = 1 ||
     test "${LJ_TEST_RUN_LOCK_HELD:-}" = 1; then
    return
  fi
  lock_timeout=${LJ_TEST_RUN_LOCK_TIMEOUT:-900}
  lock_started=$(date +%s)
  lock_announced=0
  while ! mkdir "$lock_dir" 2>/dev/null; do
    lock_now=$(date +%s)
    if test "$lock_timeout" -ge 0 &&
       test $((lock_now-lock_started)) -ge "$lock_timeout"; then
      echo "ARM64 dynamic-args NUM contract lock timed out: $lock_dir" >&2
      if test -f "$lock_dir/owner"; then
        echo "owner:" >&2
        cat "$lock_dir/owner" >&2 || true
      fi
      exit 2
    fi
    if test "$lock_announced" = 0; then
      echo "waiting for Lua test runner lock: $lock_dir" >&2
      lock_announced=1
    fi
    sleep 0.2
  done
  lock_held=1
  {
    printf 'time=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'cmd=%s\n' "$0"
  } >"$lock_dir/owner" 2>/dev/null || true
}

require_fixture_sequence() {
  region_start=$1
  region_end=$2
  required=$3
  region=$(
    awk -v start="$region_start" -v finish="$region_end" '
      !copy && index($0, start) { copy=1 }
      copy && index($0, finish) { exit }
      copy { print }
    ' "$fixture_source" | tr '\n\t' '  ' | tr -s ' '
  )
  case "$region" in
    *"$required"*) ;;
    *)
      echo "ARM64 dynamic-args NUM fixture lost scoped proof: $required" >&2
      exit 1
      ;;
  esac
}

test -f "$fixture_source"
for required in \
  'function __arm64_pure_numeric_args(x,limit,step)' \
  'while x<limit do x=x+step end return x end' \
  'function __arm64_pure_numeric_args_inclusive(x,limit,step)' \
  'while x<=limit do x=x+step end return x end' \
  'function __arm64_pure_numeric_args_add_descending(x,limit,step)' \
  'while x>limit do x=x+step end return x end' \
  'function __arm64_pure_numeric_args_descending(x,limit,step)' \
  'while x>limit do x=x-step end return x end' \
  'function __arm64_pure_numeric_args_descending_inclusive' \
  '(x,limit,step) while x>=limit do x=x-step end return x end' \
  'NUMERIC_ARGS_STRICT, BC_ISGE, 3, 4, BC_ADDVV, IR_ADD,' \
  'IR_GT, IR_LT, A64I_FADDd, 0, CC_HS, CC_LO,' \
  'NUMERIC_ARGS_INCLUSIVE, BC_ISGT, 3, 4, BC_ADDVV, IR_ADD,' \
  'IR_GE, IR_LE, A64I_FADDd, 0, CC_HI, CC_LS,' \
  'NUMERIC_ARGS_STRICT, BC_ISGE, 4, 3, BC_ADDVV, IR_ADD,' \
  'IR_LT, IR_GT, A64I_FADDd, 1, CC_HS, CC_LO,' \
  'NUMERIC_ARGS_STRICT, BC_ISGE, 4, 3, BC_SUBVV, IR_SUB,' \
  'IR_LT, IR_GT, A64I_FSUBd, 1, CC_HS, CC_LO,' \
  'NUMERIC_ARGS_INCLUSIVE, BC_ISGT, 4, 3, BC_SUBVV, IR_SUB,' \
  'IR_LE, IR_GE, A64I_FSUBd, 1, CC_HI, CC_LS,' \
  'pt->framesize == 5 && pt->sizebc == 13 && pt->numparams == 3' \
  'pt->sizeuv == 0 && pt->sizekn == 0 && pt->sizekgc == 0' \
  'pt->flags == PROTO_HAS_RETURN' \
  'pt->flags2 == PROTO2_CELLOPS' \
  'profile->compare_a, profile->compare_d' \
  'bc_op(ins) == profile->recurrence_bc && bc_a(ins) == 3' \
  'assert(pc == proto_bc(pt)+5u);' \
  '#define QNAN_BITS UINT64_C(0x7ff8000000000000)' \
  '#define PINF_BITS UINT64_C(0x7ff0000000000000)' \
  '#define NINF_BITS UINT64_C(0xfff0000000000000)' \
  'static const MSize expected_mapofs[] = { 0, 2, 6, 9, 12 };' \
  'static const uint8_t expected_nslots[] = { 5, 6, 5, 5, 5 };' \
  'static const uint8_t expected_pcpos[] = { 6, 2, 11, 6, 11 };' \
  'static const uint8_t expected_map_slots[] = { 2, 5, 2, 2, 2 };' \
  'assert(trace_nk_acq(T) == REF_TRUE);' \
  'expect_ir(ir, R_X, IR_SLOAD, IRT_NUM|IRT_GUARD,' \
  'expect_ir(ir, R_STEP, IR_SLOAD, IRT_NUM|IRT_GUARD,' \
  'expect_ir(ir, R_X_PRE, profile->recurrence_ir,' \
  'IRT_NUM|IRT_ISPHI, R_X, R_STEP);' \
  'IRT_NUM|IRT_ISPHI, R_STEP, R_X);' \
  'expect_ir(ir, R_LIMIT, IR_SLOAD, IRT_NUM|IRT_GUARD,' \
  'expect_ir(ir, R_PRECOND, profile->precondition_op,' \
  'expect_ir(ir, R_X_BODY, profile->recurrence_ir, IRT_NUM|IRT_ISPHI,' \
  'expect_ir(ir, R_COND, profile->body_op,' \
  'expect_ir(ir, R_X_PHI, IR_PHI, IRT_NUM, R_X_PRE, R_X_BODY);' \
  'assert(fpr_index(x) == 2);' \
  'assert(fpr_index(step) == 1);' \
  'assert(fpr_index(xpre) == 15);' \
  'assert(fpr_index(limit) == 0);' \
  'assert(xpre == xbody && xpre == xphi);' \
  'assert(step != xphi && limit != xphi && step != limit);' \
  'assert(x != step);' \
  'if ((ins & farith_mask) == profile->recurrence_mcode)' \
  'profile->evolution == NUMERIC_ARGS_SUB_DESCENDING' \
  'assert(right == stepreg);' \
  'profile->recurrence_mcode == A64I_FADDd ? A64I_FSUBd : A64I_FADDd' \
  'if (profile->fcmp_limit_first)' \
  'assert(left == limitreg && right == phireg);' \
  'assert(left == phireg && right == limitreg);' \
  'assert(nfarith == 2 && nopposite == 0);' \
  'assert(nfcmp == 2 && npre == 1 && nbody == 1);' \
  'trace_szmcode_acq(T) == 140 && trace_mcloop_acq(T) == 80' \
  'trace_szmcode_acq(T) == 136 && trace_mcloop_acq(T) == 76' \
  'profile->precondition_exit_cc' \
  'profile->body_loop_cc' \
  'POSTADMISSION_PROFILE' \
  'POSTADMISSION_STOPREQ' \
  'POSTADMISSION_QNAN_X' \
  'POSTADMISSION_PINF_X' \
  'POSTADMISSION_NINF_X' \
  'POSTADMISSION_QNAN_LIMIT' \
  'POSTADMISSION_PINF_LIMIT' \
  'POSTADMISSION_NINF_LIMIT' \
  'POSTADMISSION_QNAN_STEP' \
  'POSTADMISSION_PINF_STEP' \
  'POSTADMISSION_NINF_STEP' \
  '.stop_after_mutation = 1' \
  'target = &base[0];' \
  'target = &base[1];' \
  'target = &base[2];' \
  'tv_rawstore_rel(target, replacement);' \
  'isnan(result)' \
  'isinf(result) && result > 0' \
  'isinf(result) && result < 0' \
  'MUTATION_NINF' \
  'MUTATION_FINITE, 0.75' \
  'MUTATION_FINITE, 19.75' \
  '{ 20.5, 0.25, -0.5, 0.0 },' \
  '{ 0.5, -0.625, -0.375, -0.625 },' \
  '{ 20.25, 0.25, -0.5, 0.25 },' \
  '{ 20.0, 0.25, -0.5, 0.0 },' \
  '{ 20.5, 0.25, -1.0, -0.5 },' \
  '{ 20.5, 1.0, -0.5, 1.0 },' \
  '{ 0.75, 0.5, -0.5, 0.25 }' \
  '** 0.125, or -1.0 respectively instead of -0.625.' \
  '{ 0.5, -0.625, 0.375, -0.625 },' \
  '{ 0.375, -0.625, 0.25, -0.875 },' \
  '{ 20.25, 0.25, 0.5, -0.25 },' \
  '** -0.75, 0.125, or -1.125 respectively instead of -0.875.' \
  'profile->reuse.x, profile->reuse.limit, profile->reuse.step,' \
  '1.0, 0.25, equality_body_step, 0, 0, 0) == 0.25' \
  '1.0, 0.25, 0.375, 0, 0, 0) == -0.125' \
  '1.0, 0.5, equality_first_step, 0, 0, 0) == 0.5' \
  '1.0, 0.5, 0.5, 0, 0, 0) == 0.0' \
  '0.5, 0.5, equality_first_step, 0, 0, 0) == 0.5' \
  '0.5, 0.5, 0.5, 0, 0, 0) == 0.0' \
  '0.625, 1.0, 0.375, 0, 0, 0) == 1.375' \
  '1.0, 1.0, 0.375, 0, 0, 0) == 1.375' \
  'expect_native_exit(X_OR_STEP_TYPE_EXIT, X_OR_STEP_TYPE_EXIT);' \
  'expect_native_exit(LIMIT_TYPE_EXIT, LIMIT_TYPE_EXIT);' \
  'expect_native_exit(PRECOND_EXIT, PRECOND_EXIT);' \
  'expect_profile_exit_and_reentry();' \
  'thread interrupted: VM shutdown' \
  'void *saved_cframe = L->cframe;' \
  'assert(L->cframe == saved_cframe);' \
  'trace_nchild_acq(T) == 0 && trace_nextside_acq(T) == 0' \
  'function __arm64_fixed_initializer(limit,step) local x=0.5' \
  'function __arm64_fixed_half(limit) local x=0.5' \
  'function __arm64_fixed_initializer_inclusive(limit,step) local x=0.5' \
  'function __arm64_fixed_half_inclusive(limit) local x=0.5' \
  'function __arm64_fixed_initializer_add_descending(limit,step)' \
  'function __arm64_fixed_half_add_descending(limit) local x=20.5' \
  'expect_no_trace(L, "__arm64_fixed_initializer_add_descending");' \
  'expect_no_trace(L, "__arm64_fixed_half_add_descending");' \
  'function __arm64_fixed_initializer_descending(limit,step) local x=20.5' \
  'function __arm64_fixed_half_descending(limit) local x=20.5' \
  'function __arm64_fixed_initializer_descending_inclusive(limit,step)' \
  'function __arm64_fixed_half_descending_inclusive(limit) local x=20.5' \
  'expect_no_trace(L, "__arm64_fixed_initializer_descending_inclusive");' \
  'expect_no_trace(L, "__arm64_fixed_half_descending_inclusive");' \
  'function __arm64_args_sub_lt(x,limit,step)' \
  'while x<limit do x=x-step end return x end' \
  'expect_no_trace(L, "__arm64_args_sub_lt");' \
  'while x<limit do x=x*step end return x end' \
  'while x<limit do x=x/step end return x end' \
  'function __arm64_args_add_ge(x,limit,step)' \
  'while x>=limit do x=x+step end return x end' \
  'expect_no_trace(L, "__arm64_args_add_ge");' \
  'while limit>=x do x=x+step end return x end' \
  'while x<=limit do x=step+x end return x end' \
  'while x<limit do x=x+step+step end return x end' \
  'while x<=limit do x=x+step+step end return x end' \
  'function __arm64_args_reversed_add_gt_compare(x,limit,step)' \
  'while limit<x do x=x+step end return x end' \
  'expect_no_trace(L, "__arm64_args_reversed_add_gt_compare");' \
  'function __arm64_args_reversed_add_gt(x,limit,step)' \
  'while x>limit do x=step+x end return x end' \
  'expect_no_trace(L, "__arm64_args_reversed_add_gt");' \
  'function __arm64_args_extra_add_gt(x,limit,step)' \
  'while x>limit do x=x+step+step end return x end' \
  'expect_no_trace(L, "__arm64_args_extra_add_gt");' \
  'function __arm64_args_add_gt_mul(x,limit,step)' \
  'expect_no_trace(L, "__arm64_args_add_gt_mul");' \
  'function __arm64_args_add_gt_div(x,limit,step)' \
  'expect_no_trace(L, "__arm64_args_add_gt_div");' \
  'function __arm64_args_sub_le(x,limit,step)' \
  'while x<=limit do x=x-step end return x end' \
  'expect_no_trace(L, "__arm64_args_sub_le");' \
  'function __arm64_args_descending_mul(x,limit,step)' \
  'while x>limit do x=x*step end return x end' \
  'function __arm64_args_descending_inclusive_mul(x,limit,step)' \
  'while x>=limit do x=x*step end return x end' \
  'expect_no_trace(L, "__arm64_args_descending_inclusive_mul");' \
  'function __arm64_args_descending_div(x,limit,step)' \
  'while x>limit do x=x/step end return x end' \
  'function __arm64_args_descending_inclusive_div(x,limit,step)' \
  'while x>=limit do x=x/step end return x end' \
  'expect_no_trace(L, "__arm64_args_descending_inclusive_div");' \
  'function __arm64_args_reversed_descending_compare(x,limit,step)' \
  'while limit<x do x=x-step end return x end' \
  'function __arm64_args_reversed_descending_inclusive_compare' \
  '(x,limit,step) while limit<=x do x=x-step end return x end' \
  'expect_no_trace(L, "__arm64_args_reversed_descending_inclusive_compare");' \
  'function __arm64_args_reversed_sub(x,limit,step)' \
  'while x>limit do x=step-x end return x end' \
  'function __arm64_args_reversed_inclusive_sub(x,limit,step)' \
  'while x>=limit do x=step-x end return x end' \
  'expect_no_trace(L, "__arm64_args_reversed_inclusive_sub");' \
  'function __arm64_args_extra_sub(x,limit,step)' \
  'while x>limit do x=x-step-step end return x end' \
  'function __arm64_args_extra_inclusive_sub(x,limit,step)' \
  'while x>=limit do x=x-step-step end return x end' \
  'expect_no_trace(L, "__arm64_args_extra_inclusive_sub");' \
  'test_positive_and_guard_exits(&strict_profile);' \
  'test_positive_and_guard_exits(&inclusive_profile);' \
  'test_positive_and_guard_exits(&add_descending_profile);' \
  'test_positive_and_guard_exits(&descending_profile);' \
  'test_positive_and_guard_exits(&descending_inclusive_profile);' \
  'run_lua(L, "jit.flush()")' \
  'proto_trace_acq(pt) == 0'; do
  grep -F "$required" "$fixture_source" >/dev/null || {
    echo "ARM64 dynamic-args NUM fixture lost proof: $required" >&2
    exit 1
  }
done

require_fixture_sequence \
  'static int numeric_args_is_descending' \
  'static const NumericArgsProfile strict_profile' \
  'return profile->evolution == NUMERIC_ARGS_ADD_DESCENDING || profile->evolution == NUMERIC_ARGS_SUB_DESCENDING;'

for required in \
  'if (profile->evolution == NUMERIC_ARGS_SUB_DESCENDING) { expect_ir(ir, R_X_PRE, profile->recurrence_ir, IRT_NUM|IRT_ISPHI, R_X, R_STEP);' \
  '} else { expect_ir(ir, R_X_PRE, profile->recurrence_ir, IRT_NUM|IRT_ISPHI, R_STEP, R_X);'; do
  require_fixture_sequence \
    'static void expect_ir_shape' \
    'static void expect_snapshot_shape' "$required"
done

for required in \
  'if (profile->evolution == NUMERIC_ARGS_SUB_DESCENDING) { assert(right == stepreg);' \
  '} else { unsigned other; assert((left == stepreg) != (right == stepreg)); other = left == stepreg ? right : left;'; do
  require_fixture_sequence \
    'static void expect_dynamic_fp_mcode' \
    'static void expect_only_args_root' "$required"
done

# Keep the new profile's exact bytecode/IR/machine-code certificate and all
# value-sensitive rows together. Global token checks would allow fields from
# unrelated profiles to satisfy this proof accidentally.
for required in \
  '"__arm64_pure_numeric_args_add_descending", NUMERIC_ARGS_ADD_DESCENDING,' \
  'NUMERIC_ARGS_STRICT, BC_ISGE, 4, 3, BC_ADDVV, IR_ADD, IR_LT, IR_GT, A64I_FADDd, 1, CC_HS, CC_LO,' \
  '{ 20.5, 0.25, -0.5, 0.0 }, { 0.5, -0.625, -0.375, -0.625 }, { 20.5, 0.25, -0.5, 0.0 }, { 20.25, 0.25, -0.5, 0.25 }, { 20.0, 0.25, -0.5, 0.0 }, { 20.5, 0.25, -1.0, -0.5 }, { 20.5, 1.0, -0.5, 1.0 }, { 0.75, 0.5, -0.5, 0.25 }'; do
  require_fixture_sequence \
    'static const NumericArgsProfile add_descending_profile = {' \
    'static const NumericArgsProfile descending_profile = {' "$required"
done

for required in \
  'if (profile->evolution == NUMERIC_ARGS_ADD_DESCENDING) { run_lua(L,' \
  '"function __arm64_pure_numeric_args_add_descending(x,limit,step) " "while x>limit do x=x+step end return x end");'; do
  require_fixture_sequence \
    'static void test_positive_and_guard_exits' \
    'assert(call_triple(L, profile->name,' "$required"
done

for required in \
  'assert(call_triple(L, profile->name, profile->reuse.x, profile->reuse.limit, profile->reuse.step, 0, 0, 0) == profile->reuse.result); expect_single_exit(FINAL_EXIT);' \
  'if (profile->evolution == NUMERIC_ARGS_ADD_DESCENDING) {' \
  'assert(call_triple(L, profile->name, profile->record.x, profile->reuse.limit, profile->reuse.step, 0, 0, 0) == -0.875); expect_single_exit(FINAL_EXIT);' \
  'assert(call_triple(L, profile->name, profile->reuse.x, profile->record.limit, profile->reuse.step, 0, 0, 0) == 0.125); expect_single_exit(PRECOND_EXIT);' \
  'assert(call_triple(L, profile->name, profile->reuse.x, profile->reuse.limit, profile->record.step, 0, 0, 0) == -1.0); expect_single_exit(FINAL_EXIT);'; do
  require_fixture_sequence \
    '/* The same trace must consume different accumulator' \
    'if (profile->evolution == NUMERIC_ARGS_SUB_DESCENDING &&' "$required"
done

for required in \
  'if (numeric_args_is_descending(profile)) { test_terminating_mutation(L, pt, idle_vmstate, profile, POSTADMISSION_QNAN_X, profile->mutation.x, MUTATION_QNAN, 0.0);' \
  'test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile, POSTADMISSION_PINF_X, profile->mutation.x);' \
  'test_terminating_mutation(L, pt, idle_vmstate, profile, POSTADMISSION_NINF_X, profile->mutation.x, MUTATION_NINF, 0.0);' \
  'test_terminating_mutation(L, pt, idle_vmstate, profile, POSTADMISSION_QNAN_LIMIT, profile->mutation.limit, MUTATION_FINITE, 19.75);' \
  'test_terminating_mutation(L, pt, idle_vmstate, profile, POSTADMISSION_PINF_LIMIT, profile->mutation.limit, MUTATION_FINITE, 19.75);' \
  'test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile, POSTADMISSION_NINF_LIMIT, profile->mutation.limit);' \
  'test_terminating_mutation(L, pt, idle_vmstate, profile, POSTADMISSION_QNAN_STEP, profile->mutation.step, MUTATION_QNAN, 0.0);' \
  'if (profile->evolution == NUMERIC_ARGS_ADD_DESCENDING) { test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile, POSTADMISSION_PINF_STEP, profile->mutation.step);' \
  'test_terminating_mutation(L, pt, idle_vmstate, profile, POSTADMISSION_NINF_STEP, profile->mutation.step, MUTATION_NINF, 0.0);' \
  '} else { test_terminating_mutation(L, pt, idle_vmstate, profile, POSTADMISSION_PINF_STEP, profile->mutation.step, MUTATION_NINF, 0.0);' \
  'test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile, POSTADMISSION_NINF_STEP, profile->mutation.step);'; do
  require_fixture_sequence \
    'test_xpoll_lifecycle(L, pt, idle_vmstate, profile);' \
    'if (profile->evolution == NUMERIC_ARGS_ADD_ASCENDING &&' "$required"
done

for required in \
  'if (profile->evolution == NUMERIC_ARGS_SUB_DESCENDING && profile->comparison == NUMERIC_ARGS_INCLUSIVE) {' \
  'assert(call_triple(L, profile->name, 1.0, 0.25, 0.375, 0, 0, 0) == -0.125); expect_single_exit(FINAL_EXIT);' \
  'assert(call_triple(L, profile->name, 1.0, 0.5, 0.5, 0, 0, 0) == 0.0); expect_single_exit(FINAL_EXIT);' \
  'assert(call_triple(L, profile->name, 0.5, 0.5, 0.5, 0, 0, 0) == 0.0); expect_single_exit(PRECOND_EXIT);'; do
  require_fixture_sequence \
    '/* The same trace must consume different accumulator' \
    'test_xpoll_lifecycle(L, pt, idle_vmstate, profile);' "$required"
done

for required in \
  '} else if (numeric_args_is_descending(profile)) {' \
  'profile->evolution == NUMERIC_ARGS_SUB_DESCENDING ? 0.375 : -0.375;' \
  'profile->evolution == NUMERIC_ARGS_SUB_DESCENDING ? 0.5 : -0.5;' \
  'assert(call_triple(L, profile->name, 1.0, 0.25, equality_body_step, 0, 0, 0) == 0.25); expect_single_exit(FINAL_EXIT);' \
  'assert(call_triple(L, profile->name, 1.0, 0.5, equality_first_step, 0, 0, 0) == 0.5); expect_single_exit(PRECOND_EXIT);' \
  'assert(call_triple(L, profile->name, 0.5, 0.5, equality_first_step, 0, 0, 0) == 0.5); assert(lj_trace_test_root_entry_publishes() == 0); assert(lj_trace_test_exit_calls() == 0);'; do
  require_fixture_sequence \
    '} else if (numeric_args_is_descending(profile)) {' \
    '} else if (profile->comparison == NUMERIC_ARGS_INCLUSIVE) {' "$required"
done

for required in \
  '"function __arm64_fixed_initializer_add_descending(limit,step) " "local x=20.5 while x>limit do x=x+step end return x end " "assert(__arm64_fixed_initializer_add_descending(0.25,-0.5)==0.0)"); expect_no_trace(L, "__arm64_fixed_initializer_add_descending");' \
  '"function __arm64_fixed_half_add_descending(limit) local x=20.5 " "while x>limit do x=x+(-0.5) end return x end " "assert(__arm64_fixed_half_add_descending(0.25)==0.0)"); pt = global_proto(L, "__arm64_fixed_half_add_descending"); expect_no_trace(L, "__arm64_fixed_half_add_descending");'; do
  require_fixture_sequence \
    'static void test_fixed_initializers_remain_separate' \
    'static void test_sub_lt_rejected' "$required"
done

for required in \
  '"function __arm64_args_add_ge(x,limit,step) " "while x>=limit do x=x+step end return x end");' \
  '"function __arm64_args_reversed_add_gt_compare(x,limit,step) " "while limit<x do x=x+step end return x end");' \
  '"function __arm64_args_reversed_add_gt(x,limit,step) " "while x>limit do x=step+x end return x end");' \
  '"function __arm64_args_extra_add_gt(x,limit,step) " "while x>limit do x=x+step+step end return x end");' \
  '"function __arm64_args_add_gt_mul(x,limit,step) " "while x>limit do x=x*step end return x end");' \
  '"function __arm64_args_add_gt_div(x,limit,step) " "while x>limit do x=x/step end return x end");'; do
  require_fixture_sequence \
    'static void test_add_descending_adjacent_rejected' \
    'static void test_extra_add_rejected' "$required"
done

test "$(grep -Fc 'test_positive_and_guard_exits(&' \
  "$fixture_source")" -eq 5 || {
  echo "ARM64 dynamic-args NUM fixture lost a positive profile" >&2
  exit 1
}
for profile in strict_profile inclusive_profile add_descending_profile \
  descending_profile \
  descending_inclusive_profile; do
  test "$(grep -Fc \
    "test_positive_and_guard_exits(&$profile);" "$fixture_source")" -eq 1 || {
    echo "ARM64 dynamic-args NUM fixture lost exact $profile invocation" >&2
    exit 1
  }
done

# Keep the fail-closed proof bodies live: source-token checks alone would not
# detect main() silently ceasing to execute one of their containing suites.
main_region=$(
  awk '
    /^int main\(int argc, char \*\*argv\)/ { copy=1 }
    copy { print }
    copy && /^#else/ { exit }
  ' "$fixture_source"
)
test -n "$main_region"
test "$(printf '%s\n' "$main_region" | \
  grep -Fc 'test_positive_and_guard_exits(&')" -eq 5 || {
  echo "ARM64 dynamic-args NUM main lost a positive profile" >&2
  exit 1
}
for profile in strict_profile inclusive_profile add_descending_profile \
  descending_profile descending_inclusive_profile; do
  test "$(printf '%s\n' "$main_region" | grep -Fc \
    "test_positive_and_guard_exits(&$profile);")" -eq 1 || {
    echo "ARM64 dynamic-args NUM main lost exact $profile invocation" >&2
    exit 1
  }
done
for suite in \
  test_fixed_initializers_remain_separate \
  test_sub_lt_rejected \
  test_mul_rejected \
  test_div_rejected \
  test_adjacent_comparisons_rejected \
  test_extra_add_rejected \
  test_add_descending_adjacent_rejected \
  test_descending_adjacent_rejected; do
  test "$(printf '%s\n' "$main_region" | grep -Fc "$suite();")" -eq 1 || {
    echo "ARM64 dynamic-args NUM fixture lost exact $suite invocation" >&2
    exit 1
  }
done

for name in \
  __arm64_fixed_initializer_add_descending \
  __arm64_fixed_half_add_descending \
  __arm64_args_add_ge \
  __arm64_args_reversed_add_gt_compare \
  __arm64_args_reversed_add_gt \
  __arm64_args_extra_add_gt \
  __arm64_args_add_gt_mul \
  __arm64_args_add_gt_div \
  __arm64_args_sub_lt \
  __arm64_args_sub_le; do
  test "$(grep -Fc "expect_no_trace(L, \"$name\");" \
    "$fixture_source")" -eq 1 || {
    echo "ARM64 dynamic-args NUM fixture lost exact no-trace identity: $name" >&2
    exit 1
  }
done
if grep -F 'expect_no_trace(L, "__arm64_args_gt");' \
    "$fixture_source" >/dev/null; then
  echo "ARM64 dynamic-args NUM fixture retained obsolete ADD_GT rejection" >&2
  exit 1
fi
if grep -F '"function __arm64_args_gt(' "$fixture_source" >/dev/null; then
  echo "ARM64 dynamic-args NUM fixture retained obsolete ADD_GT negative" >&2
  exit 1
fi

test "$(grep -Fc 'void *saved_cframe = L->cframe;' \
  "$fixture_source")" -eq 3 || {
  echo "ARM64 dynamic-args NUM lifecycle lost cframe baselines" >&2
  exit 1
}
test "$(grep -Fc 'assert(L->cframe == saved_cframe);' \
  "$fixture_source")" -eq 6 || {
  echo "ARM64 dynamic-args NUM lifecycle lost cframe restoration proof" >&2
  exit 1
}
test "$(grep -Fc 'assert(gc2_hs_leader_acq(g) == 0);' \
  "$fixture_source")" -eq 4 || {
  echo "ARM64 dynamic-args NUM lifecycle lost handshake cleanup proof" >&2
  exit 1
}

for required in \
  '/* LT       */ CC_GE + (CC_HS << 4),' \
  '/* GT    x  */ CC_LE + (CC_HS << 4),' \
  '/* GE    x  */ CC_LT + (CC_HI << 4),' \
  '/* LE       */ CC_GT + (CC_HI << 4),' \
  'asm_guardcc(as, (asm_compmap[ir->o] >> 4));' \
  'emit_nm(as, ai, left, right);' \
  'asm_fparith(as, ir, A64I_FADDd);' \
  'asm_fparith(as, ir, A64I_FSUBd);' \
  'emit_dnm(as, ai, (dest & 31), (left & 31), (right & 31));'; do
  grep -F "$required" "$backend_source" >/dev/null || {
    echo "ARM64 dynamic-args NUM FP lowering changed: $required" >&2
    exit 1
  }
done
grep -F 'CC_HS = CC_CS, CC_LO = CC_CC' "$target_source" >/dev/null || {
  echo "ARM64 dynamic-args NUM condition aliases changed" >&2
  exit 1
}
grep -F 'CC_HI, CC_LS, CC_GE, CC_LT, CC_GT, CC_LE, CC_AL' \
  "$target_source" >/dev/null || {
  echo "ARM64 inclusive dynamic-args condition codes changed" >&2
  exit 1
}

acquire_lock
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-dynamic-args-num.XXXXXX")
fixture=$tmpdir/t-arm64-jit-pure-numeric-args
fixture_obj=$tmpdir/t-arm64-jit-pure-numeric-args.o
pauth_fixture=$tmpdir/t-arm64-jit-pure-numeric-args-arm64e
pauth_obj=$tmpdir/t-arm64-jit-pure-numeric-args-arm64e.o
macros=$tmpdir/macros.txt
pauth_macros=$tmpdir/macros-arm64e.txt
entry_helper=$tmpdir/root-entry-helper.c

restore_needed=1
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' XCFLAGS="$xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$xcflags"
test "$(lipo -archs "$archive")" = arm64
nm "$archive" | grep ' T _lj_trace_test_root_entry_reset$' >/dev/null
nm "$archive" | grep ' T _lj_trace_test_reset_exit_stats$' >/dev/null

awk '/^lj_trace_enter_root\(/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$root/src/lj_trace.c" >"$entry_helper"
test -s "$entry_helper"
final_pending=$(grep -n 'trace_root_entry_request_pending(tg)' \
  "$entry_helper" | sed -n '3p' | cut -d: -f1)
postadmission=$(grep -n 'LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION' \
  "$entry_helper" | sed -n '1p' | cut -d: -f1)
tmpbuf=$(grep -n 'setsbufL(&tg->tmpbuf, L)' "$entry_helper" | \
  sed -n '1p' | cut -d: -f1)
test -n "$final_pending" && test -n "$postadmission" && test -n "$tmpbuf"
test "$final_pending" -lt "$postadmission"
test "$postadmission" -lt "$tmpbuf"
test "$(grep -Fc 'trace_root_entry_request_pending(tg)' \
  "$entry_helper")" = 3

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -arch arm64 -mmacosx-version-min="$minver" $xcflags \
  -I"$root/src" -dM -E -x c -include lj_arch.h /dev/null >"$macros"
for setting in \
  'LJ_TARGET_ARM64 1' \
  'LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED 1'; do
  grep -E "^#define ${setting}$" "$macros" >/dev/null || {
    echo "ARM64 dynamic-args NUM gate mismatch: $setting" >&2
    exit 1
  }
done

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  -c "$fixture_source" -o "$fixture_obj"
"$cc" -arch arm64 -mmacosx-version-min="$minver" \
  "$fixture_obj" "$archive" -lm -pthread -o "$fixture"
"$fixture" direct
ordinary_runs=${LJ_ARM64_PURE_NUMERIC_ARGS_RUNS:-2}
run=1
while test "$run" -le "$ordinary_runs"; do
  LUAJIT_MCODE_TEST=R "$fixture" randomized
  run=$((run+1))
done

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean \
    TARGET_FLAGS='-arch arm64e -mbranch-protection=bti' \
    XCFLAGS="$pauth_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" \
    TARGET_FLAGS='-arch arm64e -mbranch-protection=bti' \
    XCFLAGS="$pauth_xcflags"

# shellcheck disable=SC2086 # pauth_xcflags intentionally expands.
"$cc" -arch arm64e -mbranch-protection=bti \
  -mmacosx-version-min="$minver" $pauth_xcflags -I"$root/src" \
  -dM -E -x c -include lj_arch.h /dev/null >"$pauth_macros"
for setting in \
  'LJ_TARGET_ARM64 1' \
  'LJ_ABI_PAUTH 1' \
  'LJ_ABI_BRANCH_TRACK 1' \
  'LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED 0'; do
  grep -E "^#define ${setting}$" "$pauth_macros" >/dev/null || {
    echo "ARM64e dynamic-args NUM gate mismatch: $setting" >&2
    exit 1
  }
done

# shellcheck disable=SC2086 # pauth_xcflags intentionally expands.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64e \
  -mbranch-protection=bti -mmacosx-version-min="$minver" $pauth_xcflags \
  -I"$root/src" -c "$fixture_source" -o "$pauth_obj"
"$cc" -arch arm64e -mbranch-protection=bti \
  -mmacosx-version-min="$minver" "$pauth_obj" "$archive" -lm -pthread \
  -o "$pauth_fixture"
otool -hv "$pauth_fixture" | grep -E 'ARM64[[:space:]]+E' >/dev/null
"$pauth_fixture" direct
pauth_runs=${LJ_ARM64_PURE_NUMERIC_ARGS_PAUTH_RUNS:-2}
run=1
while test "$run" -le "$pauth_runs"; do
  LUAJIT_MCODE_TEST=R "$pauth_fixture" randomized
  run=$((run+1))
done

echo "arm64_jit_pure_numeric_args_contract OK: ADD_LT/ADD_LE/ADD_GT/SUB_GT/SUB_GE dynamic-accumulator NUM roots and lifecycle proved on ARM64/arm64e"
