#!/bin/sh
set -eu
root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip gc2_constructor_recovery_contract; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi
lj_ci_profile_init "$root" gc2_constructor_recovery_contract
lj_ci_tmpdir lj-gc2-constructor-recovery
tmpdir=$LJ_CI_TMPDIR
export LUA_PATH="$root/tests/lib/?.lua;$root/src/?.lua;$root/src/?/init.lua;;"

run_profile() {
  profile=$1
  mode=${profile#*-}
  case "$profile" in
    arm64-*)
      arch=arm64; target='-arch arm64'
      flags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL' ;;
    arm64e-*)
      arch=arm64e; target='-arch arm64e -mbranch-protection=bti'
      flags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUAJIT_ENABLE_CET_BR' ;;
    x64-*)
      arch=x86_64; target='-arch x86_64 -mcx16'; flags='' ;;
    *) echo "unsupported constructor-recovery profile: $profile" >&2; return 2 ;;
  esac
  case "$mode" in
    assert) flags="$flags -DLUA_USE_ASSERT" ;;
    release) ;;
    paranoia) flags="$flags -DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1" ;;
    *) echo "unsupported constructor-recovery mode: $mode" >&2; return 2 ;;
  esac
  flags="$flags -DLJ_GC2_TEST_HELPERS"
  lj_ci_build_profile "$arch" "$target" "$flags" "$profile"
  lj_ci_require_symbols lj_arena_test_construct_pause lj_gc2_test_recovery_pause
  for name in gc2-recovery gc-root-pending-race gc-root-pending; do
    fixture=$tmpdir/t-$name-$profile
    lj_ci_compile_fixture "$fixture" "$root/tests/t-$name.c"
    lj_ci_run_fixture 120 "$fixture"
  done
  echo "gc2_constructor_recovery_contract passed=$profile profile=$LJ_CI_PROFILE_ID"
}

profiles=${LJ_GC2_CONSTRUCTOR_PROFILES:-'arm64-assert arm64e-assert arm64-release arm64e-release x64-assert x64-release arm64-paranoia x64-paranoia'}
set -f
for profile in $profiles; do run_profile "$profile"; done
set +f
lj_ci_restore "$LJ_CI_RESTORE_TARGET_FLAGS" "$LJ_CI_RESTORE_XCFLAGS"
echo "gc2_constructor_recovery_contract OK: constructor ownership and recovery across profiles: $profiles"
