#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip arm64_jit_rooted_read_contract; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi

lj_ci_profile_init "$root" arm64_jit_rooted_read_contract
lj_ci_tmpdir lj-arm64-rooted-read
tmpdir=$LJ_CI_TMPDIR
minver=$LJ_CI_MINVER
archive=$LJ_CI_ARCHIVE
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS -DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS'
pauth_xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR"
safeexperimental_xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT'
restore_xcflags=${LJ_ARM64_ROOTED_READ_RESTORE_XCFLAGS:-$safeexperimental_xcflags}
lj_ci_set_restore '-arch arm64' "$restore_xcflags"
admit=$root/src/lj_asm_arm64_admit.h
contract=$root/tools/ci/arm64_jit_ir_admission_contract.sh
fixture_source=$root/tests/t-arm64-jit-rooted-read-admission.c
lifecycle_source=$root/tests/t-arm64-jit-rooted-read-lifecycle.c
loop_lua=$root/tests/t-arm64-jit-rooted-read-loop.lua
test -f "$fixture_source" && test -f "$lifecycle_source" && test -f "$loop_lua"
# Malformed references, roots, helper IDs, guards, snapshots and locations
# are checked by executing independent mutation fixtures on both targets.
# The positive operation matrix deliberately varies formerly fixed shapes.
test -f "$root/tests/t-arm64-jit-rooted-operations.lua"

build_and_run() {
  arch=$1
  target_flags=$2
  flags=$3
  tag=$4
  lj_ci_build_profile "$arch" "$target_flags" "$flags" "$tag"
  if test "$tag" = arm64; then
    LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 LJ_TEST_XCFLAGS="$flags" \
      sh "$contract"
  fi
  for symbol in lj_asm_arm64_ir_admit lj_asm_arm64_postra_admit \
                lj_tab_getscalar_rooted_try lj_trace_test_root_entry_publishes; do
    nm "$archive" | grep " T _$symbol\$" >/dev/null || {
      echo "ARM64 rooted-read archive lacks native proof entry $symbol" >&2
      exit 1
    }
  done
  fixture=$tmpdir/t-arm64-jit-rooted-read-admission-$tag
  lifecycle=$tmpdir/t-arm64-jit-rooted-read-lifecycle-$tag
  lj_ci_compile_fixture "$fixture" "$fixture_source"
  lj_ci_compile_fixture "$lifecycle" "$lifecycle_source"
  LUA_PATH="$root/tests/lib/?.lua;$root/src/?.lua;$root/src/jit/?.lua;;" \
    lj_ci_run_fixture 120 "$fixture" "$loop_lua"
  LUA_PATH="$root/tests/lib/?.lua;$root/src/?.lua;$root/src/jit/?.lua;;" \
    lj_ci_run_fixture 120 "$fixture" "$root/tests/t-arm64-jit-rooted-operations.lua" multiple
  for variant in string-key constant-string boolean-result; do
    LUA_PATH="$root/tests/lib/?.lua;$root/src/?.lua;$root/src/jit/?.lua;;" \
      lj_ci_run_fixture 120 "$fixture" "$root/tests/t-arm64-jit-rooted-operations.lua" "$variant"
  done
  lj_ci_run_fixture 120 "$lifecycle"
  LJ_TEST_ROOT="$root" LUA_PATH="$root/tests/lib/?.lua;$root/src/?.lua;$root/src/jit/?.lua;;" \
    lj_ci_run_lua 120 "$loop_lua"
  LUA_PATH="$root/src/?.lua;$root/src/?/init.lua;;" \
    lj_ci_run_lua 120 "$root/tests/t-arm64-jit-rooted-operations.lua"
}

build_and_run arm64 '-arch arm64' "$xcflags" arm64
build_and_run arm64e '-arch arm64e -mbranch-protection=bti' \
  "$pauth_xcflags" arm64e

# The registered suite selects and restores its own native ARM64 profile.
# Run it before the final caller-profile restoration, including on failure.
LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
  LUA_PATH="$root/tests/lib/?.lua;$root/tests/suites/?.lua;;" \
  lj_ci_run_lua 150 "$root/tools/test.lua" m6_jit_rooted_read_ir

lj_ci_restore '-arch arm64' "$restore_xcflags"
echo "arm64_jit_rooted_read_contract OK: ARM64 and arm64e/BTI semantic/post-RA rejection, native rooted scalar helper, active-peer read lifecycle, and M6 suite registration verified"
