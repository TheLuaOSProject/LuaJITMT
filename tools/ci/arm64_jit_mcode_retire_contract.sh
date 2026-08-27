#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_mcode_retire_contract SKIP: requires native macOS arm64"
  exit 0
fi

jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
cc=${CC:-clang}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS -DLUAJIT_MCODE_TEST'
archive=$root/src/libluajit.a
fixture_source=$root/tests/t-jit-mcode-retire.c
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
      echo "ARM64 mcode-retirement contract lock timed out: $lock_dir" >&2
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
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-mcode-retire.XXXXXX")
fixture=$tmpdir/t-jit-mcode-retire
fixture_obj=$tmpdir/t-jit-mcode-retire.o
macros=$tmpdir/macros.txt
arm64_workload=$tmpdir/arm64-workload.c

# Pin the fixture to the one executable ARM64 root shape. The x64 FORL
# workload remains below the conditional and must never leak into this branch.
awk '/ARM64 retirement trace workload:/ { copying = 1 }
     copying { print }
     copying && /^#else/ { exit }' "$fixture_source" >"$arm64_workload"
test -s "$arm64_workload"
test "$(grep -Fc 'while i<n do i=i+1 x=x+i end' \
  "$arm64_workload")" = 2
grep -F 'arm64_call_integer_loop(L, "__arm64_mcode_retire_loop")' \
  "$arm64_workload" >/dev/null
grep -F 'arm64_call_integer_loop(L, "__arm64_mcode_retire_peer")' \
  "$arm64_workload" >/dev/null
if grep -E '"[[:space:]]*for[[:space:]]|BC_FORL|BC_JFORL' \
     "$arm64_workload" >/dev/null; then
  echo "ARM64 mcode-retirement workload fell back to unsupported FORL" >&2
  exit 1
fi

for required in \
  '#define MCODE_RETIRE_ARM64_ADMITTED 1' \
  'LJ_HASJIT && !LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED' \
  'LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED &&' \
  'LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED &&' \
  '!LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED &&' \
  '!LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED &&' \
  'LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED' \
  'tr = proto_trace_acq(pt);' \
  'assert(trace_runnable_acq(T, tr));' \
  'assert(trace_root_acq(T) == 0);' \
  'assert(trace_startpt_acq(T) == pt);' \
  'TRACE_ARM64_INT_LOOP_ADMITTED' \
  'assert(trace_mcode_acq(T) != NULL);' \
  'admitted_trace = arm64_admitted_root_trace(L, J,' \
  'peer_trace = arm64_admitted_root_trace(L, J,' \
  'assert(peer_trace != admitted_trace);' \
  'assert(pinned_trace == admitted_trace);'; do
  grep -F "$required" "$fixture_source" >/dev/null || {
    echo "ARM64 mcode-retirement fixture lost proof: $required" >&2
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

# Evaluate the production architecture policy under the exact build flags.
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
    echo "ARM64 mcode-retirement gate mismatch: $setting" >&2
    exit 1
  }
done

# Compile separately so warnings-as-errors covers the fixture translation unit
# itself, then link that exact object against the thin experimental archive.
# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  -c "$fixture_source" -o "$fixture_obj"
"$cc" -arch arm64 -mmacosx-version-min="$minver" \
  "$fixture_obj" "$archive" -lm -pthread -o "$fixture"
"$fixture"

# The build is already the ordinary experimental ARM64 configuration. Leave
# it intact for the next serialized contract; failures rebuild it in cleanup.
restore_needed=0

echo "arm64_jit_mcode_retire_contract OK: admitted root pin held trace and mcode through explicit epoch reclaim"
