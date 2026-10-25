#!/bin/sh
set -eu
root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip lockless_progress_contract; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi

lj_ci_profile_init "$root" lockless_progress_contract
lj_ci_tmpdir lj-lockless-progress
tmpdir=$LJ_CI_TMPDIR
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS'
lj_ci_set_restore '-arch arm64' \
  '-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT'
lj_ci_build_profile arm64 '-arch arm64' "$xcflags" lockless-progress
lj_ci_require_symbols lj_gc2_test_progress_metrics_reset \
  lj_tab_test_scalar_progress_metrics_reset lj_gc2_test_idle_reclaim_release
lj_ci_run_lua 10 -e 'assert(jit.arch == "arm64")'
fixture=$tmpdir/t-lockless-progress-metrics
lj_ci_compile_fixture "$fixture" "$root/tests/t-lockless-progress-metrics.c"
lj_ci_run_fixture 30 "$fixture"
echo "lockless_progress_contract OK: stalled-reclaimer wait boundary and table-rescan credit measured (profile $LJ_CI_PROFILE_ID)"
