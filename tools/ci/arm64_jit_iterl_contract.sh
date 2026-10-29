#!/bin/sh
set -eu
root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip arm64_jit_iterl_contract; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi
lj_ci_profile_init "$root" arm64_jit_iterl_contract
lj_ci_tmpdir lj-arm64-iterl
base='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL'
export LUA_PATH="$root/src/?.lua;$root/etc/?.lua;;"
for profile in ${LJ_ARM64_ITERL_PROFILES:-arm64-assert arm64e-assert arm64-release arm64e-release}; do
  arch=arm64 target='-arch arm64' flags="$base -DLJ_TRACE_TEST_HELPERS"
  case $profile in *-assert) flags="$flags -DLUA_USE_ASSERT" ;; esac
  case $profile in
    arm64e-*) arch=arm64e; target='-arch arm64e -mbranch-protection=bti'; flags="$flags -DLUAJIT_ENABLE_CET_BR" ;;
  esac
  case $profile in arm64-assert|arm64e-assert|arm64-release|arm64e-release) : ;;
    *) echo "unsupported ITERL profile: $profile" >&2; exit 2 ;;
  esac
  lj_ci_build_profile "$arch" "$target" "$flags" "$profile"
  fixture=$LJ_CI_TMPDIR/iterl-$profile
  lj_ci_compile_fixture "$fixture" "$root/tests/t-arm64-jit-native-iterl.c"
  lj_ci_run_fixture 120 "$fixture"
done
lj_ci_restore "$LJ_CI_RESTORE_TARGET_FLAGS" "$LJ_CI_RESTORE_XCFLAGS"
echo 'arm64_jit_iterl_contract OK: native custom ITERL roots and independent call/result replay'
