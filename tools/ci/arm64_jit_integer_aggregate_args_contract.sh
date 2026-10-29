#!/bin/sh
set -eu
root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip arm64_jit_integer_aggregate_args_contract; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi
lj_ci_profile_init "$root" arm64_jit_integer_aggregate_args_contract
lj_ci_tmpdir lj-arm64-jit-integer-aggregate
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
    *) echo "unsupported integer aggregate profile: $profile" >&2; return 2 ;;
  esac
  lj_ci_build_profile "$arch" "$target" "$profileflags" "integer-aggregate-$profile"
  lj_ci_require_symbols lj_asm_arm64_ir_admit lj_asm_arm64_postra_admit \
    lj_ffi_native_trace_test_set_finish_hook
  object=$tmpdir/integer-aggregate-$profile.o
  library=$tmpdir/integer-aggregate-$profile.dylib
  lj_ci_compile_object "$object" \
    "$root/tests/t-arm64-jit-callxs-integer-aggregate-lib.c"
  # shellcheck disable=SC2086
  $LJ_CI_CC $LJ_CI_TARGET_FLAGS -dynamiclib \
    -mmacosx-version-min="$LJ_CI_MINVER" "$object" \
    $LJ_CI_LDFLAGS $LJ_CI_TARGET_LDFLAGS -o "$library"
  test "$(lipo -archs "$library")" = "$arch"
  export LJ_ARM64_INT_AGGREGATE_SO=$library
  admission=$tmpdir/callxs-admission-$profile
  native=$tmpdir/integer-aggregate-$profile
  lj_ci_compile_fixture "$admission" \
    "$root/tests/t-arm64-jit-callxs-admission.c"
  lj_ci_compile_fixture "$native" \
    "$root/tests/t-arm64-jit-callxs-integer-aggregate.c"
  lj_ci_run_fixture 60 "$admission"
  lj_ci_run_fixture 60 "$native" \
    "$root/tests/t-arm64-jit-callxs-integer-aggregate.lua"
  lj_ci_run_lua 60 "$root/tests/t-ffi-callxs-nested-hfa.lua"
  lj_ci_run_lua 60 "$root/tests/t-ffi-callxs-nested-hfa.lua" array
}

profiles=${LJ_ARM64_INTEGER_AGGREGATE_PROFILES:-'arm64-assert arm64e-assert arm64-release arm64e-release'}
set -f
for profile in $profiles; do run_profile "$profile"; done
set +f
lj_ci_restore "$LJ_CI_RESTORE_TARGET_FLAGS" "$LJ_CI_RESTORE_XCFLAGS"
echo "arm64_jit_integer_aggregate_args_contract OK: profiles: $profiles"
