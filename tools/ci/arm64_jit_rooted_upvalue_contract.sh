#!/bin/sh
set -eu
root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip arm64_jit_rooted_upvalue_contract; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi
lj_ci_profile_init "$root" arm64_jit_rooted_upvalue_contract
lj_ci_tmpdir lj-rooted-upvalue
tmpdir=$LJ_CI_TMPDIR
flags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLJ_TRACE_TEST_HELPERS -DLJ_GC2_TEST_HELPERS -DLJ_FUNC_TEST_HELPERS'
export LUA_PATH="$root/tests/lib/?.lua;$root/src/?.lua;$root/src/?/init.lua;;"
run_profile() {
  arch=$1
  target=$2
  profileflags=$3
  label=$4
  lj_ci_build_profile "$arch" "$target" "$profileflags" "$label"
  lj_ci_require_symbols lj_func_getuv_rooted_try lj_func_storeuv_rooted_try \
    lj_asm_arm64_ir_admit lj_asm_arm64_postra_admit
  helper=$tmpdir/helper-$label
  admission=$tmpdir/admission-$label
  lj_ci_compile_fixture "$helper" "$root/tests/t-func-rooted-upvalues.c"
  lj_ci_compile_fixture "$admission" "$root/tests/t-arm64-jit-rooted-upvalue-admission.c"
  lj_ci_run_fixture 120 "$helper" "$root/tests/t-arm64-jit-rooted-upvalues.lua"
  for mode in get store get-forl store-forl literal-string literal-number \
              literal-integer literal-double-integer literal-zero literal-negzero \
              literal-nil literal-false literal-true; do
    lj_ci_run_fixture 120 "$admission" "$root/tests/t-arm64-jit-rooted-upvalues.lua" "$mode"
  done
  lj_ci_run_lua 120 "$root/tests/t-arm64-jit-rooted-upvalues.lua"
}
run_profile arm64 '-arch arm64' "$flags -DLUA_USE_ASSERT" arm64-assert
run_profile arm64e '-arch arm64e -mbranch-protection=bti' \
  "$flags -DLUA_USE_ASSERT -DLUAJIT_ENABLE_CET_BR" arm64e-assert
run_profile arm64 '-arch arm64' "$flags" arm64-release
run_profile arm64e '-arch arm64e -mbranch-protection=bti' \
  "$flags -DLUAJIT_ENABLE_CET_BR" arm64e-release
echo 'arm64_jit_rooted_upvalue_contract OK: closed-cell reads/writes, exact continuations, independent admission, GC retention and concurrent publication on ARM64/arm64e, assert/release'
