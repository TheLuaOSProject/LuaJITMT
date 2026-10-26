#!/bin/sh
set -eu
root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip arm64_jit_bitops_contract; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi
lj_ci_profile_init "$root" arm64_jit_bitops_contract
lj_ci_tmpdir lj-arm64-jit-bitops
tmpdir=$LJ_CI_TMPDIR
flags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLJ_TRACE_TEST_HELPERS'
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
    *) echo "unsupported bitops profile: $profile" >&2; return 2 ;;
  esac

  if test "${LJ_ARM64_BITOPS_BIND_EXISTING:-0}" = 1; then
    test "$profile_count" -eq 1 || {
      echo 'bind-existing requires exactly one selected profile' >&2; return 2;
    }
    lj_ci_bind_existing_profile "$arch" "$target" "$profileflags" \
      "bitops-$profile"
  else
    lj_ci_build_profile "$arch" "$target" "$profileflags" \
      "bitops-$profile"
  fi
  lj_ci_require_symbols lj_asm_arm64_ir_admit lj_asm_arm64_postra_admit \
    lj_trace_test_root_entry_publishes lj_trace_test_exit_calls \
    lj_trace_test_first_exitno
  fixture=$tmpdir/t-arm64-jit-bitops-$profile
  lj_ci_compile_fixture "$fixture" "$root/tests/t-arm64-jit-bitops.c"
  lj_ci_run_fixture 180 "$fixture" "$root/tests/t-arm64-jit-bitops.lua"
}

profiles=${LJ_ARM64_BITOPS_PROFILES:-'arm64-assert arm64e-assert arm64-release arm64e-release'}
set -f
profile_count=0
for profile in $profiles; do profile_count=$((profile_count + 1)); done
for profile in $profiles; do run_profile "$profile"; done
set +f
if test "${LJ_ARM64_BITOPS_BIND_EXISTING:-0}" != 1; then
  lj_ci_restore "${LJ_ARM64_BITOPS_RESTORE_TARGET_FLAGS:--arch arm64}" \
    "${LJ_ARM64_BITOPS_RESTORE_XCFLAGS:-$LJ_CI_RESTORE_XCFLAGS}"
fi
echo "arm64_jit_bitops_contract OK: independent semantic/post-RA bitop admission and native integer LOOP/FORL coverage across profiles: $profiles"
