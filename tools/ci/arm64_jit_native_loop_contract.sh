#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_native_loop_contract SKIP: requires native macOS arm64"
  exit 0
fi

lock_dir=$root/src/.lj-test-run.lock
lock_held=0
tmpdir=
cleanup() {
  if test "$lock_held" = 1; then
    rm -f "$lock_dir/owner"
    rmdir "$lock_dir" 2>/dev/null || true
  fi
  if test -n "$tmpdir"; then
    rm -rf "$tmpdir"
  fi
}

trap cleanup EXIT HUP INT TERM

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
      echo "ARM64 native-loop contract lock timed out: $lock_dir" >&2
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
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-native-loop.XXXXXX")

jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
cc=${CC:-clang}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS'
archive=$root/src/libluajit.a
vm_object=$root/src/lj_vm.o
fixture=$tmpdir/t-arm64-jit-native-loop
fixture_obj=$tmpdir/t-arm64-jit-native-loop.o
macros=$tmpdir/macros.txt
jloop_source=$tmpdir/vm-jloop.dasc
jloop_open=$tmpdir/vm-jloop-open.dasc
vm_disasm=$tmpdir/vm.disasm
jloop_disasm=$tmpdir/vm-jloop.disasm

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" XCFLAGS="$xcflags" clean
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" XCFLAGS="$xcflags"

test "$(lipo -archs "$archive")" = arm64
nm "$archive" | grep ' T _lj_trace_test_root_entry_reset$' >/dev/null
nm "$archive" | grep ' T _lj_trace_test_reset_exit_stats$' >/dev/null

# Evaluate the production architecture policy, rather than allowing command
# line overrides to make a closed build look open to the fixture.
# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -arch arm64 -mmacosx-version-min="$minver" $xcflags \
  -I"$root/src" -dM -E -x c -include lj_arch.h /dev/null >"$macros"
for setting in \
  'LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED 1'; do
  grep -F "#define $setting" "$macros" >/dev/null || {
    echo "ARM64 native-loop gate mismatch: $setting" >&2
    exit 1
  }
done

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  -c "$root/tests/t-arm64-jit-native-loop.c" -o "$fixture_obj"
# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  "$root/tests/t-arm64-jit-native-loop.c" "$archive" -lm -pthread \
  -o "$fixture"
"$fixture"

# The successful JLOOP path must reserve the fixed interpreter spill area
# before the authenticated trace branch. The generated ordinary-arm64 VM uses
# BR here; br_trace_auth becomes BRAA with the same ordering on arm64e.
awk '/case BC_JLOOP:/ { seen++; if (seen == 2) copy=1 }
     copy { print }
     copy && /case BC_JMP:/ { exit }' \
  "$root/src/vm_arm64.dasc" >"$jloop_source"
test -s "$jloop_source"
awk '/#if LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED/ { gate=1 }
     gate && /^#else/ { open=1; next }
     open && /^#endif/ { exit }
     open { print }' "$jloop_source" >"$jloop_open"
test "$(grep -Fc 'sub sp, sp, #16' "$jloop_open")" = 1
test "$(grep -Fc 'br_trace_auth CARG2, CRET1' "$jloop_open")" = 1
sub_line=$(grep -nF 'sub sp, sp, #16' "$jloop_open" | cut -d: -f1)
branch_line=$(grep -nF 'br_trace_auth CARG2, CRET1' "$jloop_open" | \
  cut -d: -f1)
test "$sub_line" -lt "$branch_line"

otool -tvV "$vm_object" >"$vm_disasm"
awk '/^_lj_BC_JLOOP:/ { copy=1 }
     copy { print }
     copy && /^_lj_BC_JMP:/ { exit }' "$vm_disasm" >"$jloop_disasm"
test -s "$jloop_disasm"
grep -E 'sub[[:space:]]+sp, sp, #0x10' "$jloop_disasm" >/dev/null
grep -E 'br[[:space:]]+x1' "$jloop_disasm" >/dev/null
sub_line=$(grep -nE 'sub[[:space:]]+sp, sp, #0x10' "$jloop_disasm" | \
  sed -n '1p' | cut -d: -f1)
branch_line=$(grep -nE 'br[[:space:]]+x1' "$jloop_disasm" | \
  sed -n '1p' | cut -d: -f1)
test "$sub_line" -lt "$branch_line"

# Pin each independently closed surface. The compatibility summaries are not
# a behavioral predicate for this first-loop execution contract.
grep -A40 '^void LJ_FASTCALL lj_trace_hot' "$root/src/lj_trace.c" | \
  grep -F '#if LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED' >/dev/null
grep -A20 '^void lj_trace_ins' "$root/src/lj_trace.c" | \
  grep -F '#if LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED' >/dev/null
grep -A30 '^static void trace_hotside' "$root/src/lj_trace.c" | \
  grep -F '#if LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED' >/dev/null
grep -A20 '^void LJ_FASTCALL lj_trace_stitch' "$root/src/lj_trace.c" | \
  grep -F '#if LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED' >/dev/null
grep -F '#if LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED' \
  "$jloop_source" >/dev/null

fixture_source=$root/tests/t-arm64-jit-native-loop.c
for required in \
  "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2')" \
  'while i<n do i=i+1 x=x+i end' \
  'lj_trace_test_root_entry_publishes() == 5' \
  'lj_trace_test_root_entry_cleanups() == 0' \
  'lj_trace_test_exit_calls() == 5' \
  'lj_trace_test_last_exitno() == 8' \
  'TRACE_ARM64_INT_LOOP_ADMITTED' \
  'trace_spadjust_acq(T) == 0' \
  'trace_topslot_acq(T) == (MSize)pt->framesize' \
  'szmcode == 168' \
  'trace_mcloop_acq(T) & (sizeof(MCode)-1u)' \
  'bc_op(back) == BC_JMP' \
  'bc_j(back) < 0' \
  'assert(!ra_hasspill(ir[ref].s))' \
  'R_RENAME_I' 'R_RENAME_X' \
  'assert(L->cframe == saved_cframe)'; do
  grep -F "$required" "$fixture_source" >/dev/null || {
    echo "ARM64 native-loop fixture lost required proof: $required" >&2
    exit 1
  }
done
if grep -E 'LJ_ARM64_JIT_(RECORDER_ADMISSION|NATIVE_ENTRY)_FAIL_CLOSED|LJ_ARM64_JIT_FAIL_CLOSED' \
     "$fixture_source" >/dev/null; then
  echo "ARM64 native-loop fixture uses a compatibility summary gate" >&2
  exit 1
fi
test "$(grep -Fc 'call_sum_and_check_cframe(L, 20, 210)' \
  "$fixture_source")" = 5 || {
  echo "ARM64 native-loop fixture must check exactly five repeated entries" >&2
  exit 1
}

echo "arm64_jit_native_loop_contract OK: exact integer BC_LOOP recorded, assembled, published, entered and exited natively"
