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
arch=${LJ_LOCKLESS_PROGRESS_ARCH:-arm64}
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS'
case "$arch" in
  arm64) target_flags='-arch arm64' ;;
  arm64e)
    target_flags='-arch arm64e -mbranch-protection=bti'
    xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR"
    ;;
  *) echo "unsupported lockless progress target: $arch" >&2; exit 2 ;;
esac
lj_ci_set_restore '-arch arm64' \
  '-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT'
lj_ci_build_profile "$arch" "$target_flags" "$xcflags" lockless-progress
lj_ci_require_symbols lj_gc2_test_progress_metrics_reset \
  lj_tab_test_scalar_progress_metrics_reset lj_gc2_test_idle_reclaim_release
lj_ci_run_lua 10 -e 'assert(jit.arch == "arm64" or jit.arch == "arm64e")'
fixture=$tmpdir/t-lockless-progress-metrics-$arch
lj_ci_compile_fixture "$fixture" "$root/tests/t-lockless-progress-metrics.c"
lj_ci_run_fixture 30 "$fixture"
echo "lockless_progress_contract OK: $arch stalled-reclaimer, resize/retirement try latency/retries, and table-rescan credit measured (profile $LJ_CI_PROFILE_ID)"
