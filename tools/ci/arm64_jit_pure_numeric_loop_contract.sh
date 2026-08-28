#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_pure_numeric_loop_contract SKIP: requires native macOS arm64"
  exit 0
fi

jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
cc=${CC:-clang}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS -DLUAJIT_MCODE_TEST'
pauth_xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR"
archive=$root/src/libluajit.a
fixture_source=$root/tests/t-arm64-jit-pure-numeric-loop.c
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
      echo "ARM64 pure-numeric contract lock timed out: $lock_dir" >&2
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

test -f "$fixture_source"
for required in \
  'function __arm64_pure_numeric_loop(limit) local x=0.5' \
  'while x<limit do x=x+0.5 end return x end' \
  'pt->framesize == 4 && pt->sizebc == 13 && pt->numparams == 1' \
  'assert(pc == proto_bc(pt)+6u);' \
  '#define PURE_HALF_BITS UINT64_C(0x3fe0000000000000)' \
  '#define PURE_QNAN_BITS UINT64_C(0x7ff8000000000000)' \
  'static const MSize expected_mapofs[] = { 0, 2, 6, 9, 12 };' \
  'static const uint8_t expected_nent[] = { 0, 2, 1, 1, 1 };' \
  'static const uint8_t expected_nslots[] = { 4, 5, 4, 4, 4 };' \
  'static const uint8_t expected_pcpos[] = { 7, 3, 11, 7, 11 };' \
  'assert(trace_nsnapmap_acq(T) == 15);' \
  'expect_ir(ir, R_X, IR_SLOAD, IRT_NUM|IRT_GUARD,' \
  'expect_ir(ir, R_X_PRE, IR_ADD, IRT_NUM|IRT_ISPHI, R_X, K_HALF);' \
  'expect_ir(ir, R_LIMIT, IR_SLOAD, IRT_NUM|IRT_GUARD,' \
  'expect_ir(ir, R_PRECOND, IR_GT, IRT_NUM|IRT_GUARD, R_LIMIT, R_X_PRE);' \
  'expect_ir(ir, R_X_BODY, IR_ADD, IRT_NUM|IRT_ISPHI,' \
  'expect_ir(ir, R_COND, IR_LT, IRT_NUM|IRT_GUARD, R_X_BODY, R_LIMIT);' \
  'expect_ir(ir, R_X_PHI, IR_PHI, IRT_NUM, R_X_PRE, R_X_BODY);' \
  'assert(k.r == RID_INIT && k.s == SPS_NONE);' \
  'xpre = expect_fpr(ir, R_X_PRE);' \
  'xbody = expect_fpr(ir, R_X_BODY);' \
  'xphi = expect_fpr(ir, R_X_PHI);' \
  'assert(xpre == xbody && xpre == xphi);' \
  'Reg phireg = expect_fpr(ir, R_X_PHI);' \
  'assert(value.r == phireg && !ra_hasspill(value.s));' \
  'unsigned xreg = (unsigned)(expect_fpr(ir, R_X_PHI)-RID_MIN_FPR);' \
  'unsigned limitreg = (unsigned)(expect_fpr(ir, R_LIMIT)-RID_MIN_FPR);' \
  'assert(suffix.o == IR_NOP && suffix.t.irt == IRT_NIL);' \
  'POSTADMISSION_PROFILE' \
  'POSTADMISSION_STOPREQ' \
  'POSTADMISSION_QNAN_LIMIT' \
  'tv_rawstore_rel(&base[0], PURE_QNAN_BITS);' \
  'tvisnum(&live) && tvisnan(&live)' \
  'expect_profile_exit_and_reentry();' \
  'expect_single_exit(PURE_XPOLL_EXIT);' \
  'expect_single_exit(PURE_PRECOND_EXIT);' \
  'PURE_FINAL_EXIT' \
  'thread interrupted: VM shutdown' \
  'lj_trace_test_root_entry_publishes() == 2' \
  'lj_trace_test_exit_calls() == 2' \
  'lj_tg_hs_epoch_ack_acq(tg) == epoch+1u' \
  'gc2_hs_pending_acq(g) == 0' \
  '(TGF_STOPREQ|TGF_STOPREQ_FRESH)' \
  'TRACE_ARM64_INT_LOOP_ADMITTED' \
  'trace_nchild_acq(T) == 0 && trace_nextside_acq(T) == 0' \
  '(branch & 15u) == CC_HS' \
  'nfcomp == 2 && npre == 1 && nbody == 1' \
  'function __arm64_pure_numeric_negative(limit) local x=0.25' \
  'while x<limit do x=x+0.25 end return x end' \
  'static void test_dynamic_step_separate_profile(void)' \
  'while x<limit do x=x+step end return x end' \
  'assert(pt->framesize == 5 && pt->sizebc == 14 && pt->numparams == 2);' \
  'assert(trace_nk_acq(T) == REF_TRUE);' \
  'assert(trace_nins_acq(T) == REF_BASE+12u);' \
  'test_dynamic_step_separate_profile();' \
  'while i<n do i=i+1; x=x+i end return x end' \
  'while x<limit do x=x*step end return x end' \
  'while x<=limit do x=x+0.5 end return x end' \
  'run_lua(L, "jit.flush()")' \
  'proto_trace_acq(pt) == 0'; do
  grep -F "$required" "$fixture_source" >/dev/null || {
    echo "ARM64 pure-numeric fixture lost proof: $required" >&2
    exit 1
  }
done
test "$(grep -Fc 'assert(gc2_hs_leader_acq(g) == 0);' \
  "$fixture_source")" -eq 3 || {
  echo "ARM64 pure-numeric lifecycle lost handshake-leader cleanup proof" >&2
  exit 1
}

# Pin the backend mapping used by the native decoder below. Both exact FP
# guards must use an ordered comparison whose inverse exit condition is HS;
# ARM64 sets carry for unordered comparisons, so the same branch exits on NaN.
for required in \
  '/* LT       */ CC_GE + (CC_HS << 4),' \
  '/* GT    x  */ CC_LE + (CC_HS << 4),' \
  'asm_guardcc(as, (asm_compmap[ir->o] >> 4));' \
  'emit_nm(as, ai, left, right);'; do
  grep -F "$required" "$backend_source" >/dev/null || {
    echo "ARM64 pure-numeric FP comparison lowering changed: $required" >&2
    exit 1
  }
done
grep -F 'CC_HS = CC_CS, CC_LO = CC_CC' "$target_source" >/dev/null || {
  echo "ARM64 pure-numeric HS condition alias changed" >&2
  exit 1
}

acquire_lock
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-pure-numeric.XXXXXX")
fixture=$tmpdir/t-arm64-jit-pure-numeric-loop
fixture_obj=$tmpdir/t-arm64-jit-pure-numeric-loop.o
pauth_fixture=$tmpdir/t-arm64-jit-pure-numeric-loop-arm64e
pauth_obj=$tmpdir/t-arm64-jit-pure-numeric-loop-arm64e.o
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

# A publisher released from this pause can only affect native guards/XPOLL:
# admission and the final pending-request check have already completed.
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
  'LJ_ARM64_JIT_FUNCF_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_FIRST_SIDE_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED 1'; do
  grep -E "^#define ${setting}$" "$macros" >/dev/null || {
    echo "ARM64 pure-numeric gate mismatch: $setting" >&2
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
ordinary_runs=${LJ_ARM64_PURE_NUMERIC_RUNS:-2}
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
    echo "ARM64e pure-numeric gate mismatch: $setting" >&2
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
pauth_runs=${LJ_ARM64_PURE_NUMERIC_PAUTH_RUNS:-2}
run=1
while test "$run" -le "$pauth_runs"; do
  LUAJIT_MCODE_TEST=R "$pauth_fixture" randomized
  run=$((run+1))
done

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' XCFLAGS="$xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$xcflags"
restore_needed=0

echo "arm64_jit_pure_numeric_loop_contract OK: exact half-step pure-NUM root, ordered FCMP NaN-exit polarity, FPR snapshots, XPOLL/STOPREQ reuse and ARM64e/BTI all ran; dynamic-step profile stayed isolated and remaining adjacent numeric families and sides stayed closed"
