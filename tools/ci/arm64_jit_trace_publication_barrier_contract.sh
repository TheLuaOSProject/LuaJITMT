#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
source_file=$root/src/lj_gc2.c
wrapper_source=$root/src/lj_gc.c
fixture_source=$root/tests/t-arm64-jit-trace-publication-barrier.c

bounded=$(sed -n \
  '/^static int gc2_trace_publish_active_ssb_bounded(/,/^}/p' \
  "$source_file")
checkpoint=$(sed -n \
  '/^int lj_gc2_trace_publish_checkpoint_nodrain(/,/^}/p' \
  "$source_file")
veto=$(sed -n \
  '/^static void gc2_trace_publish_veto_bounded(/,/^}/p' \
  "$source_file")
wrapper=$(sed -n \
  '/^int lj_gc_pubtrace_checkpoint_nodrain(/,/^}/p' \
  "$wrapper_source")

test -n "$bounded" && test -n "$checkpoint" && test -n "$veto" && \
  test -n "$wrapper"
if printf '%s\n%s\n%s\n%s\n' \
     "$bounded" "$checkpoint" "$veto" "$wrapper" | \
   grep -E '(^|[^[:alnum:]_])(for|while)[[:space:]]*\(' >/dev/null; then
  echo "trace publication checkpoint gained an unbounded source loop" >&2
  exit 1
fi
for forbidden in \
  'lj_mem_' 'malloc(' 'calloc(' 'realloc(' 'free(' \
  'gc2_flush_ssb(' 'lj_gc2_flush_ssb(' 'gc2_recovery_publish(' \
  'gc2_activation_pin_no_reclaim(' 'lj_gc2_smr_read_enter(' \
  'lj_gc2_handshake(' 'lj_err_' 'lua_' 'lj_vmevent'; do
  if printf '%s\n%s\n%s\n%s\n' \
       "$bounded" "$checkpoint" "$veto" "$wrapper" | \
     grep -F "$forbidden" >/dev/null; then
    echo "trace publication checkpoint gained forbidden call: $forbidden" >&2
    exit 1
  fi
done
printf '%s\n' "$bounded" | grep -F 'gc2_queue_slot_store_rel(next, o)' >/dev/null
printf '%s\n' "$bounded" | \
  grep -F 'remembered_count > (uint32_t)(next-base)' >/dev/null
printf '%s\n' "$bounded" | grep -F 'lj_tg_ssb_next_rel(tg, next + 1)' >/dev/null
printf '%s\n' "$checkpoint" | \
  grep -F 'published = gcref_acq(tv->slot[traceno])' >/dev/null
printf '%s\n' "$veto" | grep -F 'gc2_recovery_failed_rel(g, 1)' >/dev/null
printf '%s\n' "$wrapper" | \
  grep -F 'return lj_gc2_trace_publish_checkpoint_nodrain(g, traceno, body);' \
  >/dev/null
grep -F 'lj_gc_pubtrace_checkpoint_nodrain(g, 2, T)' \
  "$fixture_source" >/dev/null
grep -F 'assert(held != NULL && lj_tg_ssb_free_acq(tg) == NULL);' \
  "$fixture_source" >/dev/null
grep -F 'gc2_ssb_items_drained_acq(g) == drained_before' \
  "$fixture_source" >/dev/null

if test "${LJ_GC2_TRACEPUB_SOURCE_ONLY:-0}" = 1; then
  echo "arm64_jit_trace_publication_barrier_contract OK: bounded source contract"
  exit 0
fi

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_trace_publication_barrier_contract SKIP: requires native macOS arm64"
  exit 0
fi

if test -z "${SDKROOT:-}"; then
  SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
  export SDKROOT
fi

jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
cc=${CC:-$(xcrun --sdk macosx --find clang)}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
ordinary_xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS'
test_xcflags="$ordinary_xcflags -DLJ_GC2_TEST_HELPERS"
pauth_xcflags="$test_xcflags -DLUAJIT_ENABLE_CET_BR"
archive=$root/src/libluajit.a
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
        XCFLAGS="$ordinary_xcflags" >/dev/null 2>&1 || status=1
    env MACOSX_DEPLOYMENT_TARGET="$minver" \
      make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
        XCFLAGS="$ordinary_xcflags" >/dev/null 2>&1 || status=1
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
trap cleanup EXIT HUP INT TERM

if test "${LJ_TEST_DISABLE_RUN_LOCK:-}" != 1 && \
   test "${LJ_TEST_RUN_LOCK_HELD:-}" != 1; then
  lock_timeout=${LJ_TEST_RUN_LOCK_TIMEOUT:-900}
  lock_started=$(date +%s)
  while ! mkdir "$lock_dir" 2>/dev/null; do
    lock_now=$(date +%s)
    if test "$lock_timeout" -ge 0 && \
       test $((lock_now - lock_started)) -ge "$lock_timeout"; then
      echo "trace publication barrier lock timed out: $lock_dir" >&2
      exit 2
    fi
    sleep 0.2
  done
  lock_held=1
fi

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-tracepub.XXXXXX")
fixture=$tmpdir/t-arm64-jit-trace-publication-barrier
pauth_fixture=$tmpdir/t-arm64-jit-trace-publication-barrier-arm64e
macros=$tmpdir/macros.txt

restore_needed=1
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$test_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$test_xcflags"
test "$(lipo -archs "$archive")" = arm64

"$cc" -arch arm64 -mmacosx-version-min="$minver" $test_xcflags \
  -I"$root/src" -dM -E -x c -include lj_arch.h /dev/null >"$macros"
grep -F '#define LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED 1' "$macros" >/dev/null
grep -F '#define LJ_ARM64_JIT_FIRST_SIDE_RECORDER_FAIL_CLOSED 0' \
  "$macros" >/dev/null
nm "$archive" | grep ' T _lj_gc_pubtrace_checkpoint_nodrain$' >/dev/null
nm "$archive" | grep ' T _lj_gc2_trace_publish_checkpoint_nodrain$' >/dev/null

"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $test_xcflags -I"$root/src" \
  "$fixture_source" "$archive" -lm -pthread -o "$fixture"
run=1
while test "$run" -le "${LJ_ARM64_TRACEPUB_RUNS:-2}"; do
  "$fixture"
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
test "$(lipo -archs "$archive")" = arm64e

"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64e \
  -mbranch-protection=bti -mmacosx-version-min="$minver" \
  $pauth_xcflags -I"$root/src" "$fixture_source" "$archive" \
  -lm -pthread -o "$pauth_fixture"
run=1
while test "$run" -le "${LJ_ARM64_TRACEPUB_PAUTH_RUNS:-2}"; do
  "$pauth_fixture"
  run=$((run+1))
done

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$ordinary_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$ordinary_xcflags"
test "$(lipo -archs "$archive")" = arm64
if nm "$archive" | grep ' T _lj_gc2_test_recovery_fail_closed$' >/dev/null; then
  echo "ordinary ARM64 archive retained GC2 trace-publication test helpers" >&2
  exit 1
fi
restore_needed=0

echo "arm64_jit_trace_publication_barrier_contract OK: bounded queue/veto checkpoint ran on ARM64/ARM64e and ordinary ARM64 was restored"
