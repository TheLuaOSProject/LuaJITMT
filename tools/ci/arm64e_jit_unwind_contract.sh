#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64e_jit_unwind_contract SKIP: requires native macOS arm64"
  exit 0
fi

lock_dir=$root/src/.lj-test-run.lock
lock_held=0
restore_needed=0
tmpdir=
jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
base_xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT'
native_target_flags='-arch arm64'
cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if test "$restore_needed" = 1; then
    env MACOSX_DEPLOYMENT_TARGET="$minver" \
      make -C "$root/src" clean TARGET_FLAGS="$native_target_flags" \
        XCFLAGS="$base_xcflags" >/dev/null 2>&1 || status=1
    env MACOSX_DEPLOYMENT_TARGET="$minver" \
      make -C "$root/src" -j"$jobs" TARGET_FLAGS="$native_target_flags" \
        XCFLAGS="$base_xcflags" >/dev/null 2>&1 || status=1
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
      echo "arm64e JIT unwind contract lock timed out: $lock_dir" >&2
      exit 2
    fi
    if test "$lock_announced" = 0; then
      echo "waiting for Lua test runner lock: $lock_dir" >&2
      lock_announced=1
    fi
    sleep 0.2
  done
  lock_held=1
  printf 'cmd=%s\n' "$0" >"$lock_dir/owner" 2>/dev/null || true
}

acquire_lock
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64e-jit-unwind.XXXXXX")

cc=${CC:-clang}
test_xcflags="$base_xcflags -DLJ_ERR_UNWIND_TEST_HELPERS"
target_flags='-arch arm64e -mbranch-protection=bti'
archive=$root/src/libluajit.a
err_object=$root/src/lj_err.o
fixture=$tmpdir/t-arm64e-jit-unwind
macros=$tmpdir/macros.txt
undefs=$tmpdir/lj-err-undefs.txt
restore_needed=1

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS="$target_flags" \
    XCFLAGS="$test_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS="$target_flags" \
    XCFLAGS="$test_xcflags"

# shellcheck disable=SC2086 # flag groups intentionally expand to arguments.
"$cc" -arch arm64e -mbranch-protection=bti \
  -mmacosx-version-min="$minver" $test_xcflags \
  -DLUAJIT_UNWIND_EXTERNAL -I"$root/src" -dM -E -x c \
  -include lj_arch.h /dev/null >"$macros"
for setting in \
  'LJ_ABI_PAUTH 1' \
  'LJ_TARGET_ARM64 1' \
  'LJ_UNWIND_EXT 1' \
  'LJ_UNWIND_JIT 1' \
  'LJ_ERR_UNWIND_TEST_HELPERS 1'; do
  grep -E "^#define ${setting% *}[[:space:]]+${setting#* }$" \
    "$macros" >/dev/null || {
    echo "arm64e JIT unwind macro mismatch: $setting" >&2
    exit 1
  }
done

# shellcheck disable=SC2086 # flag groups intentionally expand to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64e \
  -mbranch-protection=bti -mmacosx-version-min="$minver" \
  $test_xcflags -DLUAJIT_UNWIND_EXTERNAL -I"$root/src" \
  "$root/tests/t-arm64e-jit-unwind.c" "$archive" -lm -pthread \
  -o "$fixture"
otool -hv "$fixture" | grep -E 'ARM64[[:space:]]+E' >/dev/null
"$fixture"

nm -u "$err_object" >"$undefs"
grep -E '^___register_frame$' "$undefs" >/dev/null
grep -E '^___deregister_frame$' "$undefs" >/dev/null
grep -E '^__Unwind_SetIP$' "$undefs" >/dev/null
if grep -E '^__Unwind_Find_FDE$' "$undefs" >/dev/null; then
  echo "arm64e lj_err.o still calls the cursor-unsafe _Unwind_Find_FDE" >&2
  exit 1
fi
nm "$archive" | grep ' T _lj_err_test_arm64e_unwind_arm$' >/dev/null
nm "$archive" | grep ' T _lj_err_test_arm64e_unwind_disarm$' >/dev/null

err_source=$root/src/lj_err.c
fixture_source=$root/tests/t-arm64e-jit-unwind.c
grep -F '#if !(LJ_TARGET_OSX && LJ_TARGET_ARM64 && LJ_ABI_PAUTH)' \
  "$err_source" >/dev/null
grep -F 'ptrauth_key_return_address' "$err_source" >/dev/null
grep -F '_Unwind_GetGR(ctx, LJ_ERR_UNWIND_SP_REG)' "$err_source" >/dev/null
grep -F '_Unwind_SetIP(ctx, err_unwind_sign_ip(ctx, stub))' \
  "$err_source" >/dev/null
grep -F 'blraaz x16' "$fixture_source" >/dev/null
grep -F 'PROBE_COUNTS = 0x010101' "$fixture_source" >/dev/null
grep -F "error('arm64e interpreter unwind')" "$fixture_source" >/dev/null

# Leave the shared checkout in its ordinary native experimental mode.
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS="$native_target_flags" \
    XCFLAGS="$base_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS="$native_target_flags" \
    XCFLAGS="$base_xcflags"
restore_needed=0

echo "arm64e_jit_unwind_contract OK: interpreter and registered JIT personalities installed authenticated landings"
