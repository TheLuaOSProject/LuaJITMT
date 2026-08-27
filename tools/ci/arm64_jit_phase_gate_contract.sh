#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_phase_gate_contract SKIP: requires native macOS arm64"
  exit 0
fi

jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
cc=${CC:-clang}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS -DLUAJIT_MCODE_TEST'
pauth_xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR"
archive=$root/src/libluajit.a
fixture_source=$root/tests/t-arm64-jit-phase-gates.c
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
       test $((lock_now - lock_started)) -ge "$lock_timeout"; then
      echo "ARM64 phase-gate contract lock timed out: $lock_dir" >&2
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

acquire_lock
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-phase-gate.XXXXXX")
fixture=$tmpdir/t-arm64-jit-phase-gates
fixture_obj=$tmpdir/t-arm64-jit-phase-gates.o
pauth_fixture=$tmpdir/t-arm64-jit-phase-gates-arm64e
pauth_obj=$tmpdir/t-arm64-jit-phase-gates-arm64e.o
macros=$tmpdir/macros.txt
pauth_macros=$tmpdir/macros-arm64e.txt

# Keep the behavioral proof pinned to three new universes and the integer-loop
# root shape owned by this fixture. Garbage for SWEEP is created from C, so the
# fixture cannot silently depend on an unsupported allocating trace.
test "$(grep -Fc 'StrictLoop loop = strict_loop_new();' \
  "$fixture_source")" = 3
test "$(grep -Fc 'while i<n do i=i+1 x=x+i end' \
  "$fixture_source")" = 1
if grep -E 'require\(|ffi\.|"[[:space:]]*for[[:space:]]' \
     "$fixture_source" >/dev/null; then
  echo "ARM64 phase-gate fixture gained an unsupported Lua workload" >&2
  exit 1
fi
for required in \
  'LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION' \
  'lj_gc2_reclaim_retired(ctx->g, ctx->epoch)' \
  'LJ_GC2_SMR_META_EXCLUSIVE' \
  'lj_trace_test_root_entry_publishes() == 0' \
  'lj_trace_test_root_entry_cleanups() == 0' \
  'lj_trace_test_exit_calls() == 0' \
  'expected_publishes = kind == GATE_CLOSE_MARK ? 2u : 1u' \
  'expected_exits = kind == GATE_CLOSE_MARK ? 2u : 1u' \
  'expected_last = kind == GATE_CLOSE_MARK ? 8u : 5u' \
  'lj_trace_test_first_exitno() == 5' \
  'lj_trace_test_last_exitno() == expected_last' \
  'lj_trace_test_root_entry_startins_calls() == 0' \
  'lj_trace_test_root_entry_startins_calls() != 0' \
  'gc2_jit_sweep_displaced_acq(loop.g) == 1' \
  'lj_gc2_jit_mark_request_exit(ctx->g)' \
  'lj_gc2_jit_sweep_request_exit(ctx->g)' \
  'saw_jit_base_after' \
  'request_words_clean' \
  'closer->sweep_displaced) == 1' \
  'closer->elapsed_ns < CLOSE_FAST_NS' \
  'gc2_hs_epoch_acq(g) > closer->epoch' \
  'gc2_hs_epoch_acq(g) == closer->epoch' \
  'lj_tg_hs_epoch_ack_acq(tg) == closer->ack' \
  'lj_tg_reqmask_acq(tg) == 0' \
  'lj_tg_poll_acq(tg) == 0' \
  'lj_tg_profile_request_acq(tg) == 0' \
  'gc2_jit_mark_resume_acq(g) == gc2_cycle_acq(g)' \
  'gc2_sweep_bridge_ready_acq(g) != 0' \
  'TRACE_ARM64_INT_LOOP_ADMITTED' \
  'expect_ir(ir, R_XPOLL, IR_XPOLL' \
  'assert(!ra_hasspill(ir[ref].s))' \
  'seed_sweep_garbage(loop.L)' \
  'lua_createtable(L, 8, 0)' \
  'GateWatchdogCtx watchdog'; do
  grep -F "$required" "$fixture_source" >/dev/null || {
    echo "ARM64 phase-gate fixture lost required proof: $required" >&2
    exit 1
  }
done

restore_needed=1
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' XCFLAGS="$xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$xcflags"

test "$(lipo -archs "$archive")" = arm64
nm "$archive" | grep ' T _lj_gc2_test_idle_reclaim_pause_after_jit_quiescence$' \
  >/dev/null
nm "$archive" | grep ' T _lj_trace_test_root_entry_startins_calls$' \
  >/dev/null
nm "$archive" | grep ' T _lj_gc2_jit_mark_request_exit$' >/dev/null
nm "$archive" | grep ' T _lj_gc2_jit_sweep_request_exit$' >/dev/null

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -arch arm64 -mmacosx-version-min="$minver" $xcflags \
  -I"$root/src" -dM -E -x c -include lj_arch.h /dev/null >"$macros"
for setting in \
  'LJ_TARGET_ARM64 1' \
  'LJ_HASJIT 1' \
  'LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_FUNCF_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED 1'; do
  grep -F "#define $setting" "$macros" >/dev/null || {
    echo "ARM64 phase-gate policy mismatch: $setting" >&2
    exit 1
  }
done

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  -c "$fixture_source" -o "$fixture_obj"
"$cc" -arch arm64 -mmacosx-version-min="$minver" \
  "$fixture_obj" "$archive" -lm -pthread -o "$fixture"
"$fixture"

# Run the identical three phase races under ARM64e. The randomized mcode hint
# additionally exercises the authenticated far-exit path when placement leaves
# direct BL range, without broadening the trace shape.
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
  'LJ_ABI_PAUTH 1' \
  'LJ_ABI_BRANCH_TRACK 1' \
  'LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_FUNCF_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED 0'; do
  grep -F "#define $setting" "$pauth_macros" >/dev/null || {
    echo "ARM64e phase-gate policy mismatch: $setting" >&2
    exit 1
  }
done

# shellcheck disable=SC2086 # pauth_xcflags intentionally expands.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64e \
  -mbranch-protection=bti -mmacosx-version-min="$minver" \
  $pauth_xcflags -I"$root/src" -c "$fixture_source" -o "$pauth_obj"
"$cc" -arch arm64e -mbranch-protection=bti \
  -mmacosx-version-min="$minver" "$pauth_obj" "$archive" \
  -lm -pthread -o "$pauth_fixture"
otool -hv "$pauth_fixture" | grep -E 'ARM64[[:space:]]+E' >/dev/null
"$pauth_fixture"
LUAJIT_MCODE_TEST=R "$pauth_fixture"

# Leave the shared checkout in the ordinary experimental ARM64 configuration.
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' XCFLAGS="$xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$xcflags"
restore_needed=0

echo "arm64_jit_phase_gate_contract OK: ARM64/ARM64e IDLE veto, MARK XPOLL/regrant and SWEEP XPOLL closure passed"
