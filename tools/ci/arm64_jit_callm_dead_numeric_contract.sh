#!/bin/sh
set -eu
root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip arm64_jit_callm_dead_numeric_contract; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi
lj_ci_profile_init "$root" arm64_jit_callm_dead_numeric_contract
lj_ci_tmpdir lj-arm64-callm-dead-numeric
tmpdir=$LJ_CI_TMPDIR
flags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLJ_TRACE_TEST_HELPERS'
export LUA_PATH="$root/tests/lib/?.lua;$root/src/?.lua;$root/src/?/init.lua;;"
for profile in arm64-assert arm64e-assert arm64-release arm64e-release; do
  case "$profile" in
    arm64-assert) arch=arm64; target='-arch arm64'; extra=-DLUA_USE_ASSERT ;;
    arm64e-assert) arch=arm64e; target='-arch arm64e -mbranch-protection=bti'; extra='-DLUA_USE_ASSERT -DLUAJIT_ENABLE_CET_BR' ;;
    arm64-release) arch=arm64; target='-arch arm64'; extra= ;;
    arm64e-release) arch=arm64e; target='-arch arm64e -mbranch-protection=bti'; extra=-DLUAJIT_ENABLE_CET_BR ;;
  esac
  lj_ci_build_profile "$arch" "$target" "$flags $extra" "$profile"
  lj_ci_require_symbols lj_trace_test_exit_calls lj_trace_test_root_entry_successes
  fixture=$tmpdir/dead-numeric-$profile
  lj_ci_compile_fixture "$fixture" "$root/tests/t-arm64-jit-lua-dead-numeric.c"
  lj_ci_run_fixture 90 "$fixture"
  lj_ci_run_lua 90 "$root/tests/t-arm64-jit-lua-dead-numeric.lua"
done
lj_ci_restore "$LJ_CI_RESTORE_TARGET_FLAGS" "$LJ_CI_RESTORE_XCFLAGS"
echo 'arm64_jit_callm_dead_numeric_contract OK: four profiles; dead numeric terminal/chain/division CALLM roots, selected native exits across GC with live peer, same-code reuse, independent snapshot binding and allocated/spilled/renamed-user rejection'
