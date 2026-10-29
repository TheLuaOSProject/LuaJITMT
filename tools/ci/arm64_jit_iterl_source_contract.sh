#!/bin/sh
set -eu
root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip arm64_jit_iterl_source_contract; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi
lj_ci_profile_init "$root" arm64_jit_iterl_source_contract
lj_ci_tmpdir lj-arm64-iterl-source
base='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL'
for arch in arm64 arm64e; do
  lj_ci_build_profile "$arch" "-arch $arch" "$base -DLUA_USE_ASSERT" "$arch"
  fixture=$LJ_CI_TMPDIR/iterl-source-$arch
  lj_ci_compile_fixture "$fixture" "$root/tests/t-arm64-jit-iterl-source.c"
  lj_ci_run_fixture 30 "$fixture"
done
lj_ci_restore "$LJ_CI_RESTORE_TARGET_FLAGS" "$LJ_CI_RESTORE_XCFLAGS"
echo 'arm64_jit_iterl_source_contract OK: source tuple and generation checks only'
