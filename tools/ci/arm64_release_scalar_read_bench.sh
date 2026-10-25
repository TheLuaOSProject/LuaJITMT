#!/bin/sh
set -eu
root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip arm64_release_scalar_read_bench; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi

lj_ci_profile_init "$root" arm64_release_scalar_read_bench
lj_ci_tmpdir lj-arm64-release-scalar
tmpdir=$LJ_CI_TMPDIR
# This is a non-test ARM64 bootstrap profile: no assertion, tracing, JIT
# admission, or test-helper instrumentation macros are enabled. Bootstrap
# currently requires JIT-disabled builds; the scalar C read remains available.
restore_flags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT'
lj_ci_set_restore '-arch arm64' "$restore_flags"
lj_ci_build_profile arm64 '-arch arm64' \
  '-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_DISABLE_JIT' release-rooted-scalar
lj_ci_require_symbols lj_tab_getscalar_rooted_try
fixture=$tmpdir/release-scalar-read
lj_ci_compile_fixture "$fixture" \
  "$root/tests/t-arm64-release-scalar-read-bench.c"
lj_ci_run_fixture 30 "$fixture"
echo "arm64_release_scalar_read_bench OK: production-profile timing only (profile $LJ_CI_PROFILE_ID)"
