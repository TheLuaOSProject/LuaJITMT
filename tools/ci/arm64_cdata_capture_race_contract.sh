#!/bin/sh
set -eu
root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip arm64_cdata_capture_race_contract; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi
lj_ci_profile_init "$root" arm64_cdata_capture_race_contract
lj_ci_tmpdir lj-arm64-cdata-capture-race
tmpdir=$LJ_CI_TMPDIR
flags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLJ_TRACE_TEST_HELPERS -DLJ_XSAVE_TEST_HELPERS -DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS'
export LUA_PATH="$root/tests/lib/?.lua;$root/src/?.lua;$root/src/?/init.lua;;"
for profile in arm64-assert arm64e-assert arm64-release arm64e-release; do
  case "$profile" in
    arm64-assert) arch=arm64; target='-arch arm64'; extra=-DLUA_USE_ASSERT ;;
    arm64e-assert) arch=arm64e; target='-arch arm64e -mbranch-protection=bti'; extra='-DLUA_USE_ASSERT -DLUAJIT_ENABLE_CET_BR' ;;
    arm64-release) arch=arm64; target='-arch arm64'; extra= ;;
    arm64e-release) arch=arm64e; target='-arch arm64e -mbranch-protection=bti'; extra=-DLUAJIT_ENABLE_CET_BR ;;
  esac
  lj_ci_build_profile "$arch" "$target" "$flags $extra" "$profile"
  lj_ci_require_symbols lj_record_test_arm_cdata_call_capture
  blocker=$tmpdir/callxs-blocker-$profile.dylib
  set -f
  # shellcheck disable=SC2086
  $LJ_CI_CC -O2 -dynamiclib $LJ_CI_TARGET_FLAGS \
    -mmacosx-version-min="$LJ_CI_MINVER" \
    "$root/tests/t-ffi-callxs-remote-flush-lib.c" -o "$blocker"
  set +f
  fixture=$tmpdir/cdata-capture-race-$profile
  lj_ci_compile_fixture "$fixture" "$root/tests/t-jit-cdata-call-race.c"
  export LJ_M7_FFI_CALLXS_ARM64_SCALAR=1
  export LJ_M7_FFI_CALLXS_FLUSH_SO=$blocker
  run=1
  while test "$run" -le 3; do
    echo "arm64_cdata_capture_race_contract repetition=$run profile=$profile"
    lj_ci_run_fixture 30 "$fixture"
    run=$((run + 1))
  done
done
lj_ci_restore "$LJ_CI_RESTORE_TARGET_FLAGS" "$LJ_CI_RESTORE_XCFLAGS"
echo 'arm64_cdata_capture_race_contract OK: bounded exact-helper capture retries, exact recorder/VM stage counters, forward/reverse/nonfunction seam preservation, and three repetitions across ARM64/ARM64e assertion/release profiles'
