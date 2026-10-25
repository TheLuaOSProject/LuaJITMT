#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_rooted_read_contract SKIP: requires native macOS arm64"
  exit 0
fi

if test -z "${SDKROOT:-}"; then
  SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
  export SDKROOT
fi

jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
cc=${CC:-$(xcrun --sdk macosx --find clang)}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS -DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS'
pauth_xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR"
safeexperimental_xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT'
restore_xcflags=${LJ_ARM64_ROOTED_READ_RESTORE_XCFLAGS:-$safeexperimental_xcflags}
archive=$root/src/libluajit.a
admit=$root/src/lj_asm_arm64_admit.h
contract=$root/tools/ci/arm64_jit_ir_admission_contract.sh
fixture_source=$root/tests/t-arm64-jit-rooted-read-admission.c
lifecycle_source=$root/tests/t-arm64-jit-rooted-read-lifecycle.c
loop_lua=$root/tests/t-arm64-jit-rooted-read-loop.lua
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
        XCFLAGS="$restore_xcflags" >/dev/null 2>&1 || status=1
    env MACOSX_DEPLOYMENT_TARGET="$minver" \
      make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
        XCFLAGS="$restore_xcflags" >/dev/null 2>&1 || status=1
  fi
  if test "$lock_held" = 1; then
    rm -f "$lock_dir/owner"
    rmdir "$lock_dir" 2>/dev/null || true
  fi
  if test -n "$tmpdir"; then rm -rf "$tmpdir"; fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

if test "${LJ_TEST_DISABLE_RUN_LOCK:-0}" != 1 &&
   test "${LJ_TEST_RUN_LOCK_HELD:-0}" != 1; then
  lock_timeout=${LJ_TEST_RUN_LOCK_TIMEOUT:-900}
  lock_started=$(date +%s)
  while ! mkdir "$lock_dir" 2>/dev/null; do
    lock_now=$(date +%s)
    if test "$lock_timeout" -ge 0 &&
       test $((lock_now - lock_started)) -ge "$lock_timeout"; then
      echo "ARM64 rooted-read contract lock timed out: $lock_dir" >&2
      if test -f "$lock_dir/owner"; then cat "$lock_dir/owner" >&2 || true; fi
      exit 2
    fi
    sleep 0.2
  done
  lock_held=1
  printf 'cmd=%s\n' "$0" >"$lock_dir/owner" 2>/dev/null || true
fi

test -f "$fixture_source" && test -f "$lifecycle_source" && test -f "$loop_lua"
# Malformed references, roots, helper IDs, guards, snapshots and locations
# are checked by executing independent mutation fixtures on both targets.
# The positive operation matrix deliberately varies formerly fixed shapes.
test -f "$root/tests/t-arm64-jit-rooted-operations.lua"

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-rooted-read.XXXXXX")
restore_needed=1
build_and_run() {
  arch=$1
  target_flags=$2
  flags=$3
  tag=$4
  env MACOSX_DEPLOYMENT_TARGET="$minver" \
    make -C "$root/src" clean TARGET_FLAGS="$target_flags" XCFLAGS="$flags"
  env MACOSX_DEPLOYMENT_TARGET="$minver" \
    make -C "$root/src" -j"$jobs" TARGET_FLAGS="$target_flags" XCFLAGS="$flags"
  test "$(lipo -archs "$archive")" = "$arch" || {
    echo "ARM64 rooted-read build produced wrong archive architecture" >&2
    exit 1
  }
  if test "$tag" = arm64; then
    LJ_TEST_ROOT="$root" sh "$contract"
  fi
  for symbol in lj_asm_arm64_ir_admit lj_asm_arm64_postra_admit \
                lj_tab_getscalar_rooted_try lj_trace_test_root_entry_publishes; do
    nm "$archive" | grep " T _$symbol\$" >/dev/null || {
      echo "ARM64 rooted-read archive lacks native proof entry $symbol" >&2
      exit 1
    }
  done
  fixture=$tmpdir/t-arm64-jit-rooted-read-admission-$tag
  lifecycle=$tmpdir/t-arm64-jit-rooted-read-lifecycle-$tag
  # shellcheck disable=SC2086 # Match every fixture TU to the runtime profile.
  "$cc" -std=gnu11 -O2 -Wall -Wextra -Werror $target_flags \
    -mmacosx-version-min="$minver" $flags -I"$root/src" \
    "$fixture_source" "$archive" -lm -pthread -o "$fixture"
  # shellcheck disable=SC2086 # Match every fixture TU to the runtime profile.
  "$cc" -std=gnu11 -O2 -Wall -Wextra -Werror $target_flags \
    -mmacosx-version-min="$minver" $flags -I"$root/src" \
    "$lifecycle_source" "$archive" -lm -pthread -o "$lifecycle"
  env LUA_PATH="$root/tests/lib/?.lua;$root/src/?.lua;$root/src/jit/?.lua;;" \
    perl -MTime::HiRes=alarm -e 'alarm shift; exec @ARGV; die "exec: $!\n"' \
      120 "$fixture" "$loop_lua"
  env LUA_PATH="$root/tests/lib/?.lua;$root/src/?.lua;$root/src/jit/?.lua;;" \
    perl -MTime::HiRes=alarm -e 'alarm shift; exec @ARGV; die "exec: $!\n"' \
      120 "$fixture" "$root/tests/t-arm64-jit-rooted-operations.lua" multiple
  perl -MTime::HiRes=alarm -e 'alarm shift; exec @ARGV; die "exec: $!\n"' \
    120 "$lifecycle"
  env LJ_TEST_ROOT="$root" LUA_PATH="$root/tests/lib/?.lua;$root/src/?.lua;$root/src/jit/?.lua;;" \
    perl -MTime::HiRes=alarm -e 'alarm shift; exec @ARGV; die "exec: $!\n"' \
      120 "$root/src/luajit" "$loop_lua"
  env LUA_PATH="$root/src/?.lua;$root/src/?/init.lua;;" \
    perl -MTime::HiRes=alarm -e 'alarm shift; exec @ARGV; die "exec: $!\n"' \
      120 "$root/src/luajit" "$root/tests/t-arm64-jit-rooted-operations.lua"
}

build_and_run arm64 '-arch arm64' "$xcflags" arm64
build_and_run arm64e '-arch arm64e -mbranch-protection=bti' \
  "$pauth_xcflags" arm64e

# The registered suite selects and restores its own native ARM64 profile.
# Run it before the final caller-profile restoration, including on failure.
env LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
  LUA_PATH="$root/tests/lib/?.lua;$root/tests/suites/?.lua;;" \
  perl -MTime::HiRes=alarm -e 'alarm shift; exec @ARGV; die "exec: $!\n"' \
    150 "$root/src/luajit" "$root/tools/test.lua" m6_jit_rooted_read_ir

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$restore_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$restore_xcflags"
restore_needed=0
echo "arm64_jit_rooted_read_contract OK: ARM64 and arm64e/BTI semantic/post-RA rejection, native rooted scalar helper, active-peer read lifecycle, and M6 suite registration verified"
