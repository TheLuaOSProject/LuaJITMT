#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
CDPATH= cd -- "$root"
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip arm64_jit_string_len_contract; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi

lj_ci_profile_init "$root" arm64_jit_string_len_contract
lj_ci_tmpdir lj-arm64-string-len
tmpdir=$LJ_CI_TMPDIR
archive=$LJ_CI_ARCHIVE
runtime=$LJ_CI_RUNTIME
base_xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLJ_TRACE_TEST_HELPERS -DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS'
assert_xcflags="$base_xcflags -DLUA_USE_ASSERT"
restore_xcflags=${LJ_ARM64_STRING_LEN_RESTORE_XCFLAGS:-$LJ_CI_RESTORE_XCFLAGS}
lj_ci_set_restore "$LJ_CI_RESTORE_TARGET_FLAGS" "$restore_xcflags"
fixture_source=$root/tests/t-arm64-jit-string-len-admission.c
lua_source=$root/tests/t-arm64-jit-string-len.lua
test -f "$fixture_source" && test -f "$lua_source"

build_and_run() {
  arch=$1
  target_flags=$2
  flags=$3
  tag=$4
  lj_ci_build_profile "$arch" "$target_flags" "$flags" "$tag"
  lj_ci_require_symbols lj_asm_arm64_ir_admit lj_asm_arm64_postra_admit \
    lj_trace_test_root_entry_successes lj_trace_test_exit_calls
  fixture=$tmpdir/t-arm64-jit-string-len-$tag
  lj_ci_compile_fixture "$fixture" "$fixture_source"
  lj_ci_run_fixture 120 "$fixture"
  LUA_PATH="$root/src/?.lua;$root/src/jit/?.lua;;" \
    lj_ci_run_lua 120 "$lua_source"
}

build_and_run arm64 '-arch arm64' "$assert_xcflags" arm64-assert
build_and_run arm64e '-arch arm64e -mbranch-protection=bti' \
  "$assert_xcflags -DLUAJIT_ENABLE_CET_BR" arm64e-assert
build_and_run arm64 '-arch arm64' "$base_xcflags" arm64-release
build_and_run arm64e '-arch arm64e -mbranch-protection=bti' \
  "$base_xcflags -DLUAJIT_ENABLE_CET_BR" arm64e-release

lj_ci_restore "$LJ_CI_RESTORE_TARGET_FLAGS" "$restore_xcflags"
echo "arm64_jit_string_len_contract OK: four profile-bound ARM64/arm64e native, GC, exit and independent admission runs passed"
