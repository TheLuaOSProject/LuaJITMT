#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_rooted_tablen_contract SKIP: requires native macOS arm64"
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
restore_xcflags=${LJ_ARM64_ROOTED_TABLEN_RESTORE_XCFLAGS:-$safeexperimental_xcflags}
archive=$root/src/libluajit.a
fixture_source=$root/tests/t-tab-rooted-len-try.c
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
      echo "ARM64 rooted table-length contract lock timed out: $lock_dir" >&2
      if test -f "$lock_dir/owner"; then cat "$lock_dir/owner" >&2 || true; fi
      exit 2
    fi
    sleep 0.2
  done
  lock_held=1
  printf 'cmd=%s\n' "$0" >"$lock_dir/owner" 2>/dev/null || true
fi

test -f "$fixture_source"
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-rooted-tablen.XXXXXX")
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
  test "$(lipo -archs "$archive")" = "$arch"
  for symbol in lj_asm_arm64_ir_admit lj_asm_arm64_postra_admit \
                lj_trace_test_root_entry_publishes \
                lj_tab_test_len_rooted_try_calls; do
    nm "$archive" | grep " T _$symbol\$" >/dev/null
  done
  fixture=$tmpdir/t-tab-rooted-len-try-$tag
  # shellcheck disable=SC2086 # target-specific compiler flags intentionally expand.
  "$cc" -std=gnu11 -O2 -Wall -Wextra -Werror $target_flags \
    -mmacosx-version-min="$minver" $flags -I"$root/src" "$fixture_source" \
    "$archive" -lm -pthread -o "$fixture"
  env LUA_PATH="$root/tests/lib/?.lua;$root/src/?.lua;$root/src/jit/?.lua;;" \
    perl -MTime::HiRes=alarm -e 'alarm shift; exec @ARGV; die "exec: $!\n"' \
      120 "$fixture"
}

build_and_run arm64 '-arch arm64' "$xcflags" arm64
build_and_run arm64e '-arch arm64e -mbranch-protection=bti' \
  "$pauth_xcflags" arm64e

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$restore_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$restore_xcflags"
restore_needed=0
echo "arm64_jit_rooted_tablen_contract OK: ARM64 and arm64e/BTI semantic/post-RA admission, native helper execution, STOPREQ unwind/resume, and rooted full-GC lifecycle verified"
