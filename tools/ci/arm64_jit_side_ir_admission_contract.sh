#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip arm64_jit_side_ir_admission_contract; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi

lj_ci_profile_init "$root" arm64_jit_side_ir_admission_contract
lj_ci_tmpdir lj-arm64-side-admission
tmpdir=$LJ_CI_TMPDIR
fixture_source=$root/tests/t-arm64-jit-side-adaptation.c
asm_source=$root/src/lj_asm.c
admit_source=$root/src/lj_asm_arm64_admit.h
base_flags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLJ_TRACE_TEST_HELPERS'

# Keep both independent admission passes on the actual recorder/allocator path.
grep -F 'lj_asm_arm64_side_scalar_ir_admit(' "$asm_source" >/dev/null
grep -F 'lj_asm_arm64_side_scalar_postra_admit(' "$admit_source" >/dev/null
if grep -E 'ARM64_SIDE_(K_ADDEND|R_[A-Z]+|SEMANTIC_NINS)|LJArm64SideShape|lj_asm_arm64_side_shape' \
     "$asm_source" "$admit_source"; then
  echo "obsolete exact ARM64 side-shape admission remains in production" >&2
  exit 1
fi

if test -z "${LJ_ARM64_SIDE_PROFILES+x}"; then
  # The aggregate gate invokes this contract inside its already-built profile.
  # Keep that default bound and non-rebuilding; the four-profile loop below is
  # an explicit local validation mode.
  arch=${LJ_ARM64_SIDE_ARCH:-arm64}
  if test -n "${LJ_TEST_XCFLAGS:-}"; then
    xcflags=$LJ_TEST_XCFLAGS
  else
    xcflags="$base_flags -DLUA_USE_ASSERT"
  fi
  case "$arch" in
    arm64) target_flags='-arch arm64' ;;
    arm64e)
      target_flags='-arch arm64e -mbranch-protection=bti'
      case " $xcflags " in *' -DLUAJIT_ENABLE_CET_BR '*) : ;;
        *) xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR" ;;
      esac ;;
    *) echo "unsupported ARM64 side architecture: $arch" >&2; exit 1 ;;
  esac
  lj_ci_bind_existing_profile "$arch" "$target_flags" "$xcflags" scalar-side
  archive=$root/src/libluajit.a
  for unit in lj_asm lj_trace; do
    object=$root/src/$unit.o
    test "$object" -nt "$root/src/$unit.c" &&
    test "$object" -nt "$admit_source" || {
      echo "ARM64 side admission object is stale: $unit" >&2
      exit 1
    }
    ar -p "$archive" "$unit.o" | cmp - "$object" || {
      echo "ARM64 side archive does not contain current $unit.o" >&2
      exit 1
    }
  done
  lj_ci_require_symbols lj_trace_test_exit_calls lj_trace_test_root_entry_publishes \
    lj_trace_test_root_entry_successes lj_trace_test_root_entry_cleanups \
  lj_trace_test_arm64_side_scalar_copy_contract \
  lj_mcode_test_arm64_range lj_mcode_test_arm64_expandrange \
  lj_mcode_test_arm64_range_extensions_reset \
  lj_mcode_test_arm64_range_extensions_count
  fixture=$tmpdir/t-arm64-jit-side-adaptation-bound
  lj_ci_compile_fixture "$fixture" "$fixture_source"
  lj_ci_run_fixture 90 "$fixture"
else
  for profile in $LJ_ARM64_SIDE_PROFILES; do
    arch=arm64 target_flags='-arch arm64' xcflags=$base_flags
    case "$profile" in
      arm64-assert) xcflags="$xcflags -DLUA_USE_ASSERT" ;;
      arm64e-assert)
        arch=arm64e; target_flags='-arch arm64e -mbranch-protection=bti'
        xcflags="$xcflags -DLUA_USE_ASSERT -DLUAJIT_ENABLE_CET_BR" ;;
      arm64-release) : ;;
      arm64e-release)
        arch=arm64e; target_flags='-arch arm64e -mbranch-protection=bti'
        xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR" ;;
      *) echo "unsupported ARM64 side profile: $profile" >&2; exit 1 ;;
    esac
    lj_ci_build_profile "$arch" "$target_flags" "$xcflags" "$profile"
    lj_ci_require_symbols lj_trace_test_exit_calls lj_trace_test_root_entry_publishes \
      lj_trace_test_root_entry_successes lj_trace_test_root_entry_cleanups \
      lj_trace_test_arm64_side_scalar_copy_contract \
      lj_mcode_test_arm64_range lj_mcode_test_arm64_expandrange \
      lj_mcode_test_arm64_range_extensions_reset \
      lj_mcode_test_arm64_range_extensions_count
    fixture=$tmpdir/t-arm64-jit-side-adaptation-$profile
    lj_ci_compile_fixture "$fixture" "$fixture_source"
    lj_ci_run_fixture 90 "$fixture"
  done
  lj_ci_restore "$LJ_CI_RESTORE_TARGET_FLAGS" "$LJ_CI_RESTORE_XCFLAGS"
fi

echo "arm64_jit_side_ir_admission_contract OK: native scalar side adaptation and guarded refusal verified"
