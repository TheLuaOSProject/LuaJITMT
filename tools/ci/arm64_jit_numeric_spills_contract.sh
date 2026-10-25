#!/bin/sh
set -eu
root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip arm64_jit_numeric_spills_contract; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi

lj_ci_profile_init "$root" arm64_jit_numeric_spills_contract
lj_ci_tmpdir lj-arm64-numeric-spills
tmpdir=$LJ_CI_TMPDIR
arch=${LJ_ARM64_NUMERIC_SPILLS_ARCH:-arm64}
case "$arch" in
  arm64) target_flags='-arch arm64' ;;
  arm64e) target_flags='-arch arm64e -mbranch-protection=bti' ;;
  *) echo "unsupported numeric spill target: $arch" >&2; exit 2 ;;
esac
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS'
lj_ci_set_restore '-arch arm64' \
  '-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT'
lj_ci_build_profile "$arch" "$target_flags" "$xcflags" numeric-spills
lj_ci_require_symbols lj_asm_arm64_ir_admit lj_asm_arm64_postra_admit \
  lj_trace_test_root_entry_publishes
fixture=$tmpdir/t-arm64-jit-numeric-spills
lj_ci_compile_fixture "$fixture" "$root/tests/t-arm64-jit-numeric-spills.c"
lj_ci_run_fixture 60 "$fixture" \
  "$root/tests/t-arm64-jit-numeric-spills.lua"
echo "arm64_jit_numeric_spills_contract OK: $arch aligned NUM spans, PHI copies/reloads, FMA machine code, exact multi-result exits, profile/STOPREQ, GC and flush verified under profile $LJ_CI_PROFILE_ID"
