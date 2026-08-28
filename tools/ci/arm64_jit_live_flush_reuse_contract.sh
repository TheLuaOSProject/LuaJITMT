#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_live_flush_reuse_contract SKIP: requires native macOS arm64"
  exit 0
fi

jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
cc=${CC:-clang}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS -DLUAJIT_MCODE_TEST'
pauth_xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR"
archive=$root/src/libluajit.a
fixture_source=$root/tests/t-arm64-jit-live-flush-reuse.c
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
      echo "ARM64 live-flush/reuse contract lock timed out: $lock_dir" >&2
      if test -f "$lock_dir/owner"; then
        echo "owner:" >&2
        sed -n '1,20p' "$lock_dir/owner" >&2 || true
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
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-live-flush-reuse.XXXXXX")
fixture=$tmpdir/t-arm64-jit-live-flush-reuse
fixture_obj=$tmpdir/t-arm64-jit-live-flush-reuse.o
pauth_fixture=$tmpdir/t-arm64-jit-live-flush-reuse-arm64e
pauth_obj=$tmpdir/t-arm64-jit-live-flush-reuse-arm64e.o
macros=$tmpdir/macros.txt
pauth_macros=$tmpdir/macros-arm64e.txt
entry_helper=$tmpdir/root-entry-helper.c
signal_late=$tmpdir/safepoint-signal-late.c

# Pin the deterministic boundary used by the fixture. POSTADMISSION remains
# after the helper's final request check, and the real signal hook remains
# between the ordered reqmask and poll publications.
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

awk '/^static uint32_t safepoint_signal_late\(/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$root/src/lj_safepoint.c" >"$signal_late"
test -s "$signal_late"
reqmask_line=$(grep -n 'lj_tg_reqmask_rel(tg, actions)' "$signal_late" | \
  sed -n '1p' | cut -d: -f1)
pause_line=$(grep -n 'safepoint_test_signal_pause_after_reqmask(tg)' \
  "$signal_late" | sed -n '1p' | cut -d: -f1)
poll_line=$(grep -n 'lj_tg_poll_rel(tg, 1)' "$signal_late" | \
  sed -n '1p' | cut -d: -f1)
test -n "$reqmask_line" && test -n "$pause_line" && test -n "$poll_line"
test "$reqmask_line" -lt "$pause_line"
test "$pause_line" -lt "$poll_line"

test "$(grep -Fc 'while i<n do i=i+1 x=x+i end' \
  "$fixture_source")" = 2
if grep -E 'BC_FORL|BC_JFORL|"[[:space:]]*for[[:space:]]' \
     "$fixture_source" >/dev/null; then
  echo "ARM64 live-flush/reuse fixture fell back to unsupported FORL" >&2
  exit 1
fi
for required in \
  'lj_threading_attach_wait(ctx->L)' \
  'lj_trace_flushall_hs_noevent(ctx->L)' \
  'LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION' \
  'lj_safepoint_test_signal_pause_arm(main_tg)' \
  'lj_safepoint_test_signal_pause_release()' \
  'expect_default_exit_table(g, oldT)' \
  'lj_asm_patchexit(J, oldT, 0, trace_mcode_acq(oldT))' \
  'trace_exittarget_arm64_acq(oldT, 0) == trace_mcode_acq(oldT)' \
  'trace_exittab_acq(&J->cur) == NULL' \
  'trace_exitstub_acq(&J->cur) == NULL' \
  'lj_trace_test_exittab_frees() == 1' \
  'lj_trace_test_exittab_last_free_slots() == 9' \
  'lj_trace_test_exittab_allocs() == 2' \
  'assert(lj_tg_reqmask_acq(tg) == FLUSH_ACTIONS);' \
  'assert(lj_tg_poll_acq(tg) == 0);' \
  'expect_single_native_exit(5)' \
  'retire_stamp == flush_epoch + 1u' \
  'retired_owner->retire_epoch == flush_epoch' \
  'expect_idle_reclaim_ready(g, J)' \
  'lj_gc2_handshake(g, LJ_GC2_HS_REDISPATCH)' \
  'flush_epoch + LJ_FLUSH_EPOCHS' \
  'trace_retired_head_acq(J) == NULL' \
  'mcode_retired_head_acq(J) == NULL' \
  'J->szallmcarea == 0' \
  'lj_trace_test_slot_release_calls() == 1' \
  'lj_trace_test_slot_release_clears() == 1' \
  'lj_trace_test_findfree_calls() == 1' \
  'lj_trace_test_findfree_reuses() == 1' \
  'lj_trace_test_findfree_grows() == 0' \
  'lj_trace_test_last_findfree() == 1' \
  'expect_single_native_exit(8)'; do
  grep -F "$required" "$fixture_source" >/dev/null || {
    echo "ARM64 live-flush/reuse fixture lost proof: $required" >&2
    exit 1
  }
done
if grep -E 'gc2_hs_(actions|pending|epoch)_rel|lj_tg_(reqmask|poll)_rel' \
     "$fixture_source" >/dev/null; then
  echo "ARM64 live-flush/reuse fixture manually publishes a handshake" >&2
  exit 1
fi
if grep -E 'lj_(trace|mcode)_reclaim_retired|test_idle_reclaim_enter' \
     "$fixture_source" >/dev/null; then
  echo "ARM64 live-flush/reuse fixture bypasses normal handshake reclaim" >&2
  exit 1
fi

restore_needed=1
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' XCFLAGS="$xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$xcflags"
test "$(lipo -archs "$archive")" = arm64
nm "$archive" | grep ' T _lj_trace_test_root_entry_reset$' >/dev/null
nm "$archive" | grep ' T _lj_safepoint_test_signal_pause_arm$' >/dev/null

# Evaluate the production policy under the exact ordinary ARM64 build flags.
# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -arch arm64 -mmacosx-version-min="$minver" $xcflags \
  -I"$root/src" -dM -E -x c -include lj_arch.h /dev/null >"$macros"
for setting in \
  'LJ_TARGET_ARM64 1' \
  'LJ_HASJIT 1' \
  'LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_FUNCF_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_FIRST_SIDE_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_EXIT_TARGET_SLOTS 1' \
  'LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED 1'; do
  grep -F "#define $setting" "$macros" >/dev/null || {
    echo "ARM64 live-flush/reuse gate mismatch: $setting" >&2
    exit 1
  }
done

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  -c "$fixture_source" -o "$fixture_obj"
"$cc" -arch arm64 -mmacosx-version-min="$minver" \
  "$fixture_obj" "$archive" -lm -pthread -o "$fixture"
ordinary_runs=${LJ_ARM64_LIVE_FLUSH_RUNS:-3}
run=0
while test "$run" -lt "$ordinary_runs"; do
  "$fixture"
  run=$((run + 1))
done

# Run the identical ownership/reclamation/reuse sequence with authenticated
# trace entry, BTI landing pads and authenticated exits enabled.
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
  'LJ_ARM64_JIT_EXIT_TARGET_SLOTS 1' \
  'LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED 0'; do
  grep -F "#define $setting" "$pauth_macros" >/dev/null || {
    echo "ARM64e live-flush/reuse gate mismatch: $setting" >&2
    exit 1
  }
done

# shellcheck disable=SC2086 # pauth_xcflags intentionally expands.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64e \
  -mbranch-protection=bti -mmacosx-version-min="$minver" \
  $pauth_xcflags -I"$root/src" -c "$fixture_source" -o "$pauth_obj"
"$cc" -arch arm64e -mbranch-protection=bti \
  -mmacosx-version-min="$minver" "$pauth_obj" "$archive" -lm -pthread \
  -o "$pauth_fixture"
otool -hv "$pauth_fixture" | grep -E 'ARM64[[:space:]]+E' >/dev/null
"$pauth_fixture"

# Leave the shared checkout in ordinary experimental ARM64 mode.
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' XCFLAGS="$xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$xcflags"
restore_needed=0

echo "arm64_jit_live_flush_reuse_contract OK: real ARM64/ARM64e FLUSHJ XPOLL aged and reused trace slot 1"
