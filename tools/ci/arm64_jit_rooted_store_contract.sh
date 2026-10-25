#!/bin/sh
set -eu
root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip arm64_jit_rooted_store_contract; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi
lj_ci_profile_init "$root" arm64_jit_rooted_store_contract
lj_ci_tmpdir lj-rooted-store
tmpdir=$LJ_CI_TMPDIR
flags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TAB_TEST_HELPERS -DLJ_TG_ROOT_TEST_HELPERS'
run_profile() {
  arch=$1
  target=$2
  profileflags=$3
  lj_ci_build_profile "$arch" "$target" "$profileflags" rooted-store
  lj_ci_require_symbols lj_tab_storenum_rooted lj_tab_storebool_rooted \
    lj_tab_test_set_rooted_store_hook lj_asm_arm64_postra_admit
  helper=$tmpdir/helper-$arch
  admission=$tmpdir/admission-$arch
  lj_ci_compile_fixture "$helper" "$root/tests/t-tab-rooted-store.c"
  lj_ci_compile_fixture "$admission" "$root/tests/t-arm64-jit-rooted-store-admission.c"
  lj_ci_run_fixture 60 "$helper"
  LUA_PATH="$root/tests/lib/?.lua;$root/src/?.lua;$root/src/?/init.lua;;" \
    lj_ci_run_fixture 90 "$admission" "$root/tests/t-arm64-jit-rooted-stores.lua"
  LUA_PATH="$root/tests/lib/?.lua;$root/src/?.lua;$root/src/?/init.lua;;" \
    lj_ci_run_lua 90 "$root/tests/t-arm64-jit-rooted-stores.lua"
}
run_profile arm64 '-arch arm64' "$flags"
run_profile arm64e '-arch arm64e -mbranch-protection=bti' \
  "$flags -DLUAJIT_ENABLE_CET_BR"
echo 'arm64_jit_rooted_store_contract OK: rooted lifetime, independent admission and live store effects on ARM64/arm64e'
