#!/bin/sh
set -eu
root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip arm64_jit_integer_modulo_contract; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi
lj_ci_profile_init "$root" arm64_jit_integer_modulo_contract
lj_ci_tmpdir lj-arm64-integer-modulo
base='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLJ_TRACE_TEST_HELPERS'
export LUA_PATH="$root/src/?.lua;$root/etc/?.lua;;"
profiles=${LJ_ARM64_INTEGER_MODULO_PROFILES:-'arm64-assert arm64e-assert arm64-release arm64e-release'}
lj_ci_set_restore '-arch arm64' \
  '-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT'
for profile in $profiles; do
  arch=arm64 target='-arch arm64' flags=$base
  case $profile in
    arm64-assert) flags="$flags -DLUA_USE_ASSERT" ;;
    arm64e-assert) arch=arm64e; target='-arch arm64e -mbranch-protection=bti'; flags="$flags -DLUA_USE_ASSERT -DLUAJIT_ENABLE_CET_BR" ;;
    arm64-release) : ;;
    arm64e-release) arch=arm64e; target='-arch arm64e -mbranch-protection=bti'; flags="$flags -DLUAJIT_ENABLE_CET_BR" ;;
    *) echo "unsupported integer modulo profile: $profile" >&2; exit 2 ;;
  esac
  lj_ci_build_profile "$arch" "$target" "$flags" "$profile"
  fixture=$LJ_CI_TMPDIR/integer-modulo-$profile
  lj_ci_compile_fixture "$fixture" "$root/tests/t-arm64-jit-scalar-capability.c"
  lj_ci_run_fixture 180 "$fixture" "$root/tests/t-arm64-jit-scalar-capability.lua"
  LUA_PATH="$root/src/?.lua;$root/etc/?.lua;;" \
    lj_ci_run_fixture 120 "$root/src/luajit" \
      "$root/tests/t-arm64-jit-integer-modulo.lua"
done
lj_ci_restore "$LJ_CI_RESTORE_TARGET_FLAGS" "$LJ_CI_RESTORE_XCFLAGS"
echo "arm64_jit_integer_modulo_contract OK: profiles=$profiles; INT modulo, exact nonzero divisor proofs, independent admission mutations, native exits, edge oracles and GC"
