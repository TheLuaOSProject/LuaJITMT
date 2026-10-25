#!/bin/sh
set -eu
root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip arm64_jit_rooted_gc_contract; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi
lj_ci_profile_init "$root" arm64_jit_rooted_gc_contract
lj_ci_tmpdir lj-rooted-gc
tmpdir=$LJ_CI_TMPDIR
flags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLJ_TRACE_TEST_HELPERS -DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS -DLJ_TG_ROOT_TEST_HELPERS -DLJ_UDATA_TEST_HELPERS'
export LUA_PATH="$root/tests/lib/?.lua;$root/src/?.lua;$root/src/?/init.lua;;"
run_profile() {
  arch=$1
  target=$2
  profileflags=$3
  label=$4
  lj_ci_build_profile "$arch" "$target" "$profileflags" "$label"
  lj_ci_require_symbols lj_tab_storegc_rooted lj_tab_gettv_rooted_hit_try \
    lj_tab_test_set_rooted_gc_read_hook lj_asm_arm64_postra_admit
  helper=$tmpdir/helper-$label
  admission=$tmpdir/admission-$label
  lifecycle=$tmpdir/lifecycle-$label
  deletion=$tmpdir/deletion-$label
  scalarkeys=$tmpdir/scalar-keys-$label
  lightud=$tmpdir/lightud-read-$label
  segmentmap=$tmpdir/lightud-segment-map-$label
  lj_ci_compile_fixture "$helper" "$root/tests/t-tab-rooted-store.c"
  lj_ci_compile_fixture "$admission" "$root/tests/t-arm64-jit-rooted-gc-admission.c"
  lj_ci_compile_fixture "$lifecycle" "$root/tests/t-arm64-jit-rooted-gc-lifecycle.c"
  lj_ci_compile_fixture "$deletion" "$root/tests/t-jit-raw-nil-store.c"
  lj_ci_compile_fixture "$scalarkeys" "$root/tests/t-arm64-jit-rooted-scalar-keys-admission.c"
  lj_ci_compile_fixture "$lightud" "$root/tests/t-arm64-jit-rooted-lightud-read-admission.c"
  lj_ci_compile_fixture "$segmentmap" "$root/tests/t-lightud-segment-map-race.c"
  lj_ci_run_fixture 120 "$segmentmap" "$root/tests/t-lightud-segment-map-race.lua"
  lj_ci_run_fixture 60 "$helper"
  lj_ci_run_fixture 60 "$deletion"
  lj_ci_run_fixture 120 "$scalarkeys" "$root/tests/t-arm64-jit-rooted-scalar-keys.lua"
  lj_ci_run_lua 120 "$root/tests/t-arm64-jit-rooted-scalar-keys.lua"
  for mode in matrix admission admission_sload; do
    lj_ci_run_fixture 120 "$lightud" "$root/tests/t-arm64-jit-rooted-lightud-read.lua" "$mode"
  done
  for matrix in operations values; do
    lj_ci_run_fixture 120 "$admission" "$root/tests/t-arm64-jit-rooted-gc-$matrix.lua"
    lj_ci_run_lua 120 "$root/tests/t-arm64-jit-rooted-gc-$matrix.lua"
  done
  lj_ci_run_fixture 120 "$lifecycle" "$root/tests/t-arm64-jit-rooted-gc-operations.lua"
}
run_profile arm64 '-arch arm64' "$flags -DLUA_USE_ASSERT" arm64-assert
run_profile arm64e '-arch arm64e -mbranch-protection=bti' \
  "$flags -DLUA_USE_ASSERT -DLUAJIT_ENABLE_CET_BR" arm64e-assert
run_profile arm64 '-arch arm64' "$flags" arm64-release
run_profile arm64e '-arch arm64e -mbranch-protection=bti' \
  "$flags -DLUAJIT_ENABLE_CET_BR" arm64e-release
echo 'arm64_jit_rooted_gc_contract OK: native GC reads/stores, lifetime, independent admission and full-width spills on ARM64/arm64e, assert/release'
