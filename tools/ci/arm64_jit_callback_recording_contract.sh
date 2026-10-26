#!/bin/sh
set -eu
root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip arm64_jit_callback_recording_contract; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi
lj_ci_profile_init "$root" arm64_jit_callback_recording_contract
lj_ci_tmpdir lj-callback-recording
tmpdir=$LJ_CI_TMPDIR
signalflags='-DLJ_THR_SIGNAL_TEST_HELPERS -DLJ_PROFILE_TIMER_TEST_HELPERS'
flags="-DLJ_TRACE_TEST_HELPERS -DLJ_GC2_TEST_HELPERS $signalflags"
armflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL'
export LUA_PATH="$root/tests/lib/?.lua;$root/src/?.lua;$root/src/?/init.lua;;"
run_profile() {
  arch=$1
  target=$2
  profileflags=$3
  label=$4
  lj_ci_build_profile "$arch" "$target" "$profileflags" "$label"
  lj_ci_compile_fixture "$tmpdir/overlap-$label" \
    "$root/tests/t-vmevent-recorder-overlap.c"
  lj_ci_run_fixture 30 "$tmpdir/overlap-$label"
  lj_ci_run_fixture 30 "$tmpdir/overlap-$label" profile
  for name in t-jit-event-callback-owner t-jit-flush-stream-gate \
              t-profile-pending-stop t-posix-signal-safety; do
    lj_ci_compile_fixture "$tmpdir/$name-$label" "$root/tests/$name.c"
    lj_ci_run_fixture 60 "$tmpdir/$name-$label"
  done
  # The full fixture still exposes the independent Darwin late-kernel-signal
  # shutdown race. Exercise callback cleanup here without claiming that its
  # concurrent timer lifecycle passes. The default fixture retains that case.
  lj_ci_compile_fixture "$tmpdir/profile-cleanup-$label" \
    "$root/tests/t-profile-stop-native.c"
  lj_ci_run_fixture 60 "$tmpdir/profile-cleanup-$label" --callback-cleanup
  lj_ci_run_lua 30 -joff "$root/tests/t-profile-blocked-tg.lua"
  if test "$arch" = x86_64; then
    # The existing release token fixture has an independently reproduced
    # baseline external-unwind panic. Its assertion profile remains covered;
    # callback overlap/owner/cleanup above run in both runtime modes.
    if test "$label" = x64-assert; then
      lj_ci_compile_fixture "$tmpdir/token-$label" "$root/tests/t-jit-token.c"
      lj_ci_run_fixture 60 "$tmpdir/token-$label"
    fi
    lj_ci_run_lua 30 "$root/tests/t-profile-single-trace.lua"
  else
    lj_ci_run_lua 120 "$root/tests/t-arm64-jit-rooted-upvalues.lua"
  fi
}
run_profile arm64 '-arch arm64' "$armflags $flags -DLUA_USE_ASSERT" arm64-assert
run_profile arm64e '-arch arm64e -mbranch-protection=bti' \
  "$armflags $flags -DLUA_USE_ASSERT -DLUAJIT_ENABLE_CET_BR" arm64e-assert
run_profile arm64 '-arch arm64' "$armflags $flags" arm64-release
run_profile arm64e '-arch arm64e -mbranch-protection=bti' \
  "$armflags $flags -DLUAJIT_ENABLE_CET_BR" arm64e-release
run_profile x86_64 '-arch x86_64 -mcx16' "$flags -DLUA_USE_ASSERT" x64-assert
run_profile x86_64 '-arch x86_64 -mcx16' "$flags" x64-release

# The profiler remains available without a JIT. Exercise callback error,
# STOPREQ, pending signals and blocked-peer sampling under that branch.
lj_ci_build_profile arm64 '-arch arm64' \
  "-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_DISABLE_JIT -DLUA_USE_ASSERT $signalflags" nojit
lj_ci_compile_fixture "$tmpdir/profile-nojit" "$root/tests/t-profile-stop-native.c"
lj_ci_run_fixture 60 "$tmpdir/profile-nojit" --callback-cleanup
for name in t-profile-pending-stop t-posix-signal-safety; do
  lj_ci_compile_fixture "$tmpdir/$name-nojit" "$root/tests/$name.c"
  lj_ci_run_fixture 60 "$tmpdir/$name-nojit"
done
lj_ci_run_lua 30 "$root/tests/t-profile-blocked-tg.lua" --interp-only
echo 'arm64_jit_callback_recording_contract OK: peer callback recording, owner exclusion and cleanup on ARM64/arm64e/x64, assert/release, and no-JIT profiling'
