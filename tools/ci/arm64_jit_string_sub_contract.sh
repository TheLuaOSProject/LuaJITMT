#!/bin/sh
set -eu
root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
CDPATH= cd -- "$root"
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip arm64_jit_string_sub_contract; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi
lj_ci_profile_init "$root" arm64_jit_string_sub_contract
lj_ci_tmpdir lj-arm64-string-sub
tmpdir=$LJ_CI_TMPDIR
base='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLJ_TRACE_TEST_HELPERS -DLJ_GC2_TEST_HELPERS -DLJ_STR_TEST_HELPERS'
restore_xcflags=${LJ_ARM64_STRING_SUB_RESTORE_XCFLAGS:-$LJ_CI_RESTORE_XCFLAGS}
lj_ci_set_restore "$LJ_CI_RESTORE_TARGET_FLAGS" "$restore_xcflags"
build_and_run() {
  lj_ci_build_profile "$1" "$2" "$3" "$4"
  fixture=$tmpdir/string-sub-$4
  lj_ci_compile_fixture "$fixture" "$root/tests/t-arm64-jit-string-sub-admission.c"
  LUA_PATH="$root/src/?.lua;;" lj_ci_run_fixture 120 "$fixture" "$root/tests/t-arm64-jit-string-sub.lua" forl
  LUA_PATH="$root/src/?.lua;;" lj_ci_run_fixture 120 "$fixture" "$root/tests/t-arm64-jit-string-sub.lua" while
  LUA_PATH="$root/src/?.lua;;" lj_ci_run_fixture 120 "$fixture" "$root/tests/t-arm64-jit-string-sub.lua" negative-forl
  LUA_PATH="$root/src/?.lua;;" lj_ci_run_fixture 120 "$fixture" "$root/tests/t-arm64-jit-string-sub.lua" negative-while
  for first in 1 4 32768; do
    LUA_PATH="$root/src/?.lua;;" lj_ci_run_fixture 120 "$fixture" "$root/tests/t-arm64-jit-string-sub.lua" "negative-first-$first-forl"
    LUA_PATH="$root/src/?.lua;;" lj_ci_run_fixture 120 "$fixture" "$root/tests/t-arm64-jit-string-sub.lua" "negative-first-$first-while"
    LUA_PATH="$root/src/?.lua;;" lj_ci_run_fixture 120 "$fixture" "$root/tests/t-arm64-jit-string-sub.lua" "negative-first-default-$first-forl"
    LUA_PATH="$root/src/?.lua;;" lj_ci_run_fixture 120 "$fixture" "$root/tests/t-arm64-jit-string-sub.lua" "negative-first-default-$first-while"
  done
  for pair in 1-1 4-2 4-3 4-4 32768-32768 32768-32767 32768-16385; do
    LUA_PATH="$root/src/?.lua;;" lj_ci_run_fixture 120 "$fixture" "$root/tests/t-arm64-jit-string-sub.lua" "negative-first-last-$pair-forl"
    LUA_PATH="$root/src/?.lua;;" lj_ci_run_fixture 120 "$fixture" "$root/tests/t-arm64-jit-string-sub.lua" "negative-first-last-$pair-while"
  done
  for pair in 4-5 4-6; do
    LUA_PATH="$root/src/?.lua;;" lj_ci_run_fixture 120 "$fixture" "$root/tests/t-arm64-jit-string-sub.lua" "negative-first-last-$pair-forl"
    LUA_PATH="$root/src/?.lua;;" lj_ci_run_fixture 120 "$fixture" "$root/tests/t-arm64-jit-string-sub.lua" "negative-first-last-$pair-while"
  done
  LUA_PATH="$root/src/?.lua;;" lj_ci_run_fixture 120 "$fixture" "$root/tests/t-arm64-jit-string-sub.lua" default-end
  LUA_PATH="$root/src/?.lua;;" lj_ci_run_fixture 120 "$fixture" "$root/tests/t-arm64-jit-string-sub.lua" zero-forl
  LUA_PATH="$root/src/?.lua;;" lj_ci_run_fixture 120 "$fixture" "$root/tests/t-arm64-jit-string-sub.lua" zero-while
  LUA_PATH="$root/src/?.lua;;" lj_ci_run_fixture 120 "$fixture" "$root/tests/t-arm64-jit-string-sub.lua" zero-negative-forl
  LUA_PATH="$root/src/?.lua;;" lj_ci_run_fixture 120 "$fixture" "$root/tests/t-arm64-jit-string-sub.lua" zero-negative-while
  LUA_PATH="$root/src/?.lua;;" lj_ci_run_fixture 120 "$fixture" "$root/tests/t-arm64-jit-string-sub.lua" zero-default-end
  LUA_PATH="$root/src/?.lua;;" lj_ci_run_lua 120 "$root/tests/t-arm64-jit-string-sub.lua"
}
build_and_run arm64 '-arch arm64' "$base -DLUA_USE_ASSERT" arm64-assert
build_and_run arm64e '-arch arm64e -mbranch-protection=bti' "$base -DLUA_USE_ASSERT -DLUAJIT_ENABLE_CET_BR" arm64e-assert
build_and_run arm64 '-arch arm64' "$base" arm64-release
build_and_run arm64e '-arch arm64e -mbranch-protection=bti' "$base -DLUAJIT_ENABLE_CET_BR" arm64e-release
lj_ci_restore "$LJ_CI_RESTORE_TARGET_FLAGS" "$restore_xcflags"
echo 'arm64_jit_string_sub_contract OK: four native profiles, bounds, peer, GC and independent admission'
