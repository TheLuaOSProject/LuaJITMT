#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_numeric_loop_contract SKIP: requires native macOS arm64"
  exit 0
fi

jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
cc=${CC:-clang}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS -DLUAJIT_MCODE_TEST'
pauth_xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR"
archive=$root/src/libluajit.a
fixture_source=$root/tests/t-arm64-jit-numeric-loop.c
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
      echo "ARM64 numeric-loop contract lock timed out: $lock_dir" >&2
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
  "function __arm64_numeric_loop(n,x,step) local i=0" \
  "while i<n do i=i+1; x=x+step end return x end" \
  'R_I, R_I_PRE, R_N, R_PRECOND, R_LOOP, R_I_BODY, R_COND' \
  'static const MSize expected_mapofs[] = { 0, 2, 5, 10, 14, 18, 23 };' \
  'static const uint8_t expected_nent[] = { 0, 1, 3, 2, 2, 3, 2 };' \
  'assert(trace_nsnapmap_acq(T) == 27);' \
  'expect_ir(ir, R_X, IR_SLOAD, IRT_NUM|IRT_GUARD,' \
  'expect_ir(ir, R_STEP, IR_SLOAD, IRT_NUM|IRT_GUARD,' \
  'expect_ir(ir, R_X_PRE, IR_ADD, IRT_NUM|IRT_ISPHI, R_STEP, R_X);' \
  'expect_ir(ir, R_X_BODY, IR_ADD, IRT_NUM|IRT_ISPHI,' \
  'expect_ir(ir, R_X_PHI, IR_PHI, IRT_NUM, R_X_PRE, R_X_BODY);' \
  'expect_ir(ir, R_RENAME_I, IR_RENAME, IRT_NIL, R_I_PRE, 4);' \
  'assert(trace_nk_acq(T) == REF_TRUE-1u);' \
  'assert(rset_test(RSET_FPR, reg));' \
  'assert(!ra_hasspill(ir[ref].s));' \
  'assert(!ra_hasspill(regsp_spill(rs)));' \
  'LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION' \
  'POSTADMISSION_PROFILE' \
  'POSTADMISSION_STOPREQ' \
  'lj_tg_profile_request_rel(tg, 1)' \
  'gc2_hs_actions_rel(g, LJ_GC2_HS_STOPREQ)' \
  'gc2_hs_pending_rel(g, 1)' \
  'gc2_hs_epoch_rel(g, publisher->epoch+1u)' \
  'lj_tg_reqmask_rel(tg, LJ_GC2_HS_STOPREQ)' \
  'lj_tg_poll_rel(tg, 1)' \
  'expect_profile_exit_and_reentry(NUMERIC_XPOLL_EXIT, NUMERIC_FINAL_EXIT);' \
  'expect_single_exit(NUMERIC_XPOLL_EXIT);' \
  'thread interrupted: VM shutdown' \
  'lj_trace_test_root_entry_publishes() == 2' \
  'lj_trace_test_exit_calls() == 2' \
  'lj_trace_test_first_exitno() == xpoll' \
  'lj_trace_test_last_exitno() == final' \
  'lj_tg_hs_epoch_ack_acq(tg) == epoch+1u' \
  'gc2_hs_pending_acq(g) == 0' \
  'lj_tg_profile_request_acq(tg) == 0' \
  '(TGF_STOPREQ|TGF_STOPREQ_FRESH)' \
  'TRACE_ARM64_INT_LOOP_ADMITTED' \
  'trace_nchild_acq(T) == 0 && trace_nextside_acq(T) == 0' \
  'expect_native_exit(0, 6);' \
  'function __arm64_numeric_negative(limit) local x=0.5' \
  'while x<limit do x=x+0.5 end return x end' \
  'while i<n do i=i+1; x=x+i end return x end' \
  'while i<n do i=i+1; x=x*step end return x end' \
  'run_lua(L, "jit.flush()")' \
  'proto_trace_acq(pt) == 0'; do
  grep -F "$required" "$fixture_source" >/dev/null || {
    echo "ARM64 numeric-loop fixture lost proof: $required" >&2
    exit 1
  }
done

if grep -E 'IR_(KNUM|CONV|MUL|DIV)|IRT_NUM\|IRT_GUARD.*IR_(LT|GT)' \
     "$fixture_source" >/dev/null; then
  echo "ARM64 numeric positive IR certificate gained a closed family" >&2
  exit 1
fi

acquire_lock
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-numeric-loop.XXXXXX")
fixture=$tmpdir/t-arm64-jit-numeric-loop
fixture_obj=$tmpdir/t-arm64-jit-numeric-loop.o
pauth_fixture=$tmpdir/t-arm64-jit-numeric-loop-arm64e
pauth_obj=$tmpdir/t-arm64-jit-numeric-loop-arm64e.o
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

# The pause follows the helper's final request and bytecode recheck. A request
# published there must therefore reach native IR_XPOLL, rather than being
# rejected by admission before the trace branch.
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
    echo "ARM64 numeric-loop gate mismatch: $setting" >&2
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
ordinary_runs=${LJ_ARM64_NUMERIC_RUNS:-2}
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
    echo "ARM64e numeric-loop gate mismatch: $setting" >&2
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
pauth_runs=${LJ_ARM64_NUMERIC_PAUTH_RUNS:-2}
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

echo "arm64_jit_numeric_loop_contract OK: exact spill-free mixed INT/NUM loop and FPR-restoring XPOLL lifecycle ran on ARM64 and ARM64e/BTI; KNUM/FP-compare, CONV, MUL and sides stayed closed"
