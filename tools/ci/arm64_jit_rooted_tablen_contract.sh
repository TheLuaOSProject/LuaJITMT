#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip arm64_jit_rooted_tablen_contract; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi

lj_ci_profile_init "$root" arm64_jit_rooted_tablen_contract
lj_ci_tmpdir lj-arm64-rooted-tablen
tmpdir=$LJ_CI_TMPDIR
minver=$LJ_CI_MINVER
archive=$LJ_CI_ARCHIVE
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS -DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS'
pauth_xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR"
restore_xcflags=${LJ_ARM64_ROOTED_TABLEN_RESTORE_XCFLAGS:-$LJ_CI_RESTORE_XCFLAGS}
lj_ci_set_restore '-arch arm64' "$restore_xcflags"
fixture_source=$root/tests/t-tab-rooted-len-try.c

test -f "$fixture_source"

build_and_run() {
  arch=$1
  target_flags=$2
  flags=$3
  tag=$4
  lj_ci_build_profile "$arch" "$target_flags" "$flags" "$tag"
  for symbol in lj_asm_arm64_ir_admit lj_asm_arm64_postra_admit \
                lj_trace_test_root_entry_publishes \
                lj_tab_test_len_rooted_try_calls; do
    nm "$archive" | grep " T _$symbol\$" >/dev/null
  done
  fixture=$tmpdir/t-tab-rooted-len-try-$tag
  lj_ci_compile_fixture "$fixture" "$fixture_source"
  LUA_PATH="$root/tests/lib/?.lua;$root/src/?.lua;$root/src/jit/?.lua;;" \
    lj_ci_run_fixture 120 "$fixture"
}

build_and_run arm64 '-arch arm64' "$xcflags" arm64
build_and_run arm64e '-arch arm64e -mbranch-protection=bti' \
  "$pauth_xcflags" arm64e

lj_ci_restore '-arch arm64' "$restore_xcflags"
echo "arm64_jit_rooted_tablen_contract OK: ARM64 and arm64e/BTI semantic/post-RA admission, native helper execution, STOPREQ unwind/resume, and rooted full-GC lifecycle verified"
