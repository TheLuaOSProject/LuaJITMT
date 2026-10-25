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
archive=$root/src/libluajit.a
fixture_source=$root/tests/t-arm64-jit-side-adaptation.c
asm_source=$root/src/lj_asm.c
admit_source=$root/src/lj_asm_arm64_admit.h
fixture=$tmpdir/t-arm64-jit-side-adaptation
arch=${LJ_ARM64_SIDE_ARCH:-arm64}
xcflags=${LJ_TEST_XCFLAGS:--DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS}
case "$arch" in
  arm64) target_flags='-arch arm64' ;;
  arm64e)
    target_flags='-arch arm64e -mbranch-protection=bti'
    xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR" ;;
  *) echo "unsupported ARM64 side profile: $arch" >&2; exit 1 ;;
esac
lj_ci_bind_existing_profile "$arch" "$target_flags" "$xcflags" scalar-side

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

# The native test observes the exact patched edge, successful root-entry
# handoffs, native exits, and a later guarded type mismatch.
lj_ci_require_symbols lj_trace_test_exit_calls lj_trace_test_root_entry_publishes \
  lj_trace_test_root_entry_successes lj_trace_test_root_entry_cleanups \
  lj_trace_test_arm64_side_scalar_copy_contract

# Keep both independent admission passes on the actual recorder/allocator path.
grep -F 'lj_asm_arm64_side_scalar_ir_admit(' "$asm_source" >/dev/null
grep -F 'lj_asm_arm64_side_scalar_postra_admit(' "$admit_source" >/dev/null
if grep -E 'ARM64_SIDE_(K_ADDEND|R_[A-Z]+|SEMANTIC_NINS)|LJArm64SideShape|lj_asm_arm64_side_shape' \
     "$asm_source" "$admit_source"; then
  echo "obsolete exact ARM64 side-shape admission remains in production" >&2
  exit 1
fi

lj_ci_compile_fixture "$fixture" "$fixture_source"
lj_ci_run_fixture 90 "$fixture"

echo "arm64_jit_side_ir_admission_contract OK: native scalar side adaptation and guarded refusal verified"
