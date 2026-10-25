#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64e_ffi_callback_lifecycle_contract SKIP: requires native macOS ARM64"
  exit 0
fi

lock_dir=$root/src/.lj-test-run.lock
lock_held=0
restore_needed=0
tmpdir=
jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
test_xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_DISABLE_JIT -DLUA_USE_ASSERT -DLUAJIT_ENABLE_CET_BR'
restore_xcflags=${LJ_CALLBACK_LIFECYCLE_RESTORE_XCFLAGS:-'-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_DISABLE_JIT'}
arm64_target_flags='-arch arm64'
arm64e_target_flags='-arch arm64e -mbranch-protection=bti'

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if test "$restore_needed" = 1; then
    env MACOSX_DEPLOYMENT_TARGET="$minver" \
      make -C "$root/src" clean TARGET_FLAGS="$arm64_target_flags" \
        XCFLAGS="$restore_xcflags" >/dev/null 2>&1 || status=1
    env MACOSX_DEPLOYMENT_TARGET="$minver" \
      make -C "$root/src" -j"$jobs" TARGET_FLAGS="$arm64_target_flags" \
        XCFLAGS="$restore_xcflags" >/dev/null 2>&1 || status=1
  fi
  if test "$lock_held" = 1; then
    rm -f "$lock_dir/owner"
    rmdir "$lock_dir" 2>/dev/null || true
  fi
  if test -n "$tmpdir"; then rm -rf "$tmpdir"; fi
  exit "$status"
}
trap cleanup EXIT HUP INT TERM

if test "${LJ_TEST_RUN_LOCK_HELD:-}" != 1; then
  lock_timeout=${LJ_TEST_RUN_LOCK_TIMEOUT:-900}
  lock_started=$(date +%s)
  while ! mkdir "$lock_dir" 2>/dev/null; do
    lock_now=$(date +%s)
    if test "$lock_timeout" -ge 0 &&
       test $((lock_now - lock_started)) -ge "$lock_timeout"; then
      echo "ARM64e FFI callback lifecycle contract lock timed out: $lock_dir" >&2
      exit 2
    fi
    sleep 0.2
  done
  lock_held=1
  printf 'cmd=%s\n' "$0" >"$lock_dir/owner" 2>/dev/null || true
fi

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64e-ffi-callback-life.XXXXXX")
archive=$root/src/libluajit.a
restore_needed=1

run_fixture() {
  LC_ALL=C perl -MTime::HiRes=alarm -e \
    'alarm shift; exec @ARGV; die "exec failed: $!\n"' \
    "${LJ_CALLBACK_LIFECYCLE_TIMEOUT:-30}" "$1"
}

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS="$arm64e_target_flags" \
    XCFLAGS="$test_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS="$arm64e_target_flags" \
    XCFLAGS="$test_xcflags"

cc=${CC:-clang}
cxx=${CXX:-clang++}
for fixture in \
  t-ffi-callback-nested-native \
  t-ffi-callback-owner-lifetime \
  t-ffi-callback-stopreq \
  t-ffi-callback-attached-carrier \
  t-ffi-callback-auto-attach; do
  # shellcheck disable=SC2086 # Architecture/profile groups intentionally expand.
  "$cc" -arch arm64e -mbranch-protection=bti \
    -mmacosx-version-min="$minver" -std=gnu11 -O2 -Wall -Wextra -Werror \
    $test_xcflags -I"$root/src" "$root/tests/$fixture.c" "$archive" \
    -lm -ldl -pthread -o "$tmpdir/$fixture"
  run_fixture "$tmpdir/$fixture"
done

# This fixture reads GC2 fields through inline accessors. Compile it with the
# same assertion/profile macros as the archive to keep the shared layout exact.
# shellcheck disable=SC2086 # Architecture/profile groups intentionally expand.
"$cxx" -arch arm64e -mbranch-protection=bti \
  -mmacosx-version-min="$minver" -std=gnu++11 -O2 -Wall -Wextra -Werror \
  -fexceptions -funwind-tables $test_xcflags -I"$root/src" \
  "$root/tests/t-ffi-callback-auto-unwind.cpp" "$archive" \
  -lm -ldl -pthread -o "$tmpdir/t-ffi-callback-auto-unwind"
run_fixture "$tmpdir/t-ffi-callback-auto-unwind"

echo "arm64e_ffi_callback_lifecycle_contract OK: six callback lifecycle fixtures executed with BTI"
