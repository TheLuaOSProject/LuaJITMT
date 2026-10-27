#!/bin/sh
set -eu
root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip arm64_jit_u32_contract; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi
lj_ci_profile_init "$root" arm64_jit_u32_contract
lj_ci_tmpdir lj-arm64-jit-u32
tmpdir=$LJ_CI_TMPDIR
flags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLJ_TRACE_TEST_HELPERS -DLJ_XSAVE_TEST_HELPERS -DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS'
export LUA_PATH="$root/tests/lib/?.lua;$root/src/?.lua;$root/src/?/init.lua;;"

run_profile() {
  profile=$1
  case "$profile" in
    arm64-assert)
      arch=arm64; target='-arch arm64'; profileflags="$flags -DLUA_USE_ASSERT" ;;
    arm64e-assert)
      arch=arm64e; target='-arch arm64e -mbranch-protection=bti'
      profileflags="$flags -DLUA_USE_ASSERT -DLUAJIT_ENABLE_CET_BR" ;;
    arm64-release)
      arch=arm64; target='-arch arm64'; profileflags=$flags ;;
    arm64e-release)
      arch=arm64e; target='-arch arm64e -mbranch-protection=bti'
      profileflags="$flags -DLUAJIT_ENABLE_CET_BR" ;;
    *) echo "unsupported u32 profile: $profile" >&2; return 2 ;;
  esac
  lj_ci_build_profile "$arch" "$target" "$profileflags" "u32-$profile"
  lj_ci_require_symbols lj_asm_arm64_ir_admit lj_asm_arm64_postra_admit \
    lj_ffi_native_trace_test_set_finish_hook lj_trace_test_exit_calls
  library=$tmpdir/u32-$profile.dylib
  object=$tmpdir/u32-$profile.o
  lj_ci_compile_object "$object" "$root/tests/t-arm64-jit-callxs-u32-lib.c"
  # Compiler and target flags follow the shared profile's word-list contract.
  # shellcheck disable=SC2086
  $LJ_CI_CC $LJ_CI_TARGET_FLAGS -dynamiclib \
    -mmacosx-version-min="$LJ_CI_MINVER" "$object" \
    $LJ_CI_LDFLAGS $LJ_CI_TARGET_LDFLAGS -o "$library"
  test "$(lipo -archs "$library")" = "$arch"
  library_sha=$(lj_ci_sha256 "$library")
  echo "arm64_jit_u32_contract library=$library_sha profile=$LJ_CI_PROFILE_ID"
  export LJ_M7_FFI_CALLXS_U32_SO="$library"
  for name in callxs-u32 callxs-u32-admission callxs-u32-callm-admission callxs-admission callxs-pointer-admission; do
    fixture=$tmpdir/t-arm64-jit-$name-$profile
    lj_ci_compile_fixture "$fixture" "$root/tests/t-arm64-jit-$name.c"
    lj_ci_run_fixture 60 "$fixture"
  done
  test "$(lj_ci_sha256 "$library")" = "$library_sha"
}

profiles=${LJ_ARM64_U32_PROFILES:-'arm64-assert arm64e-assert arm64-release arm64e-release'}
set -f
for profile in $profiles; do run_profile "$profile"; done
set +f
lj_ci_restore "$LJ_CI_RESTORE_TARGET_FLAGS" "$LJ_CI_RESTORE_XCFLAGS"
echo "arm64_jit_u32_contract OK: uint32 numeric results, independent admission and native lifecycle across profiles: $profiles"
