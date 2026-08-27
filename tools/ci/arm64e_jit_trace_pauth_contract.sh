#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64e_jit_trace_pauth_contract SKIP: requires native macOS arm64"
  exit 0
fi

if test -z "${SDKROOT:-}"; then
  SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
  export SDKROOT
fi

lock_dir=$root/src/.lj-test-run.lock
lock_held=0
tmpdir=
restore_needed=0
jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
cc=${CC:-$(xcrun --sdk macosx --find clang)}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
ordinary_xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS -DLUAJIT_MCODE_TEST'
pauth_xcflags="$ordinary_xcflags -DLUAJIT_ENABLE_CET_BR"

cleanup() {
  saved_status=$?
  restore_status=0
  trap - EXIT HUP INT TERM

  # An expected PAC fault must never leave the shared checkout's build in the
  # arm64e configuration. Keep the runner lock through this best-effort restore
  # and make restore failure fail an otherwise successful contract.
  if test "$restore_needed" = 1; then
    env MACOSX_DEPLOYMENT_TARGET="$minver" \
      make -C "$root/src" clean TARGET_FLAGS='-arch arm64' \
        XCFLAGS="$ordinary_xcflags" >/dev/null 2>&1 || restore_status=$?
    if test "$restore_status" = 0; then
      env MACOSX_DEPLOYMENT_TARGET="$minver" \
        make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
          XCFLAGS="$ordinary_xcflags" >/dev/null 2>&1 || restore_status=$?
    fi
  fi
  if test -n "$tmpdir"; then
    rm -rf "$tmpdir"
  fi
  if test "$lock_held" = 1; then
    rm -f "$lock_dir/owner"
    rmdir "$lock_dir" 2>/dev/null || true
  fi
  if test "$saved_status" = 0 && test "$restore_status" != 0; then
    echo "ARM64e trace-PAUTH contract could not restore arm64 build" >&2
    saved_status=$restore_status
  fi
  exit "$saved_status"
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

acquire_lock() {
  if test "${LJ_TEST_DISABLE_RUN_LOCK:-}" = 1 ||
     test "${LJ_TEST_RUN_LOCK_HELD:-}" = 1; then
    return
  fi

  lock_timeout=${LJ_TEST_RUN_LOCK_TIMEOUT:-900}
  lock_started=$(date +%s)
  lock_announced=0
  while ! mkdir "$lock_dir" 2>/dev/null; do
    lock_now=$(date +%s)
    if test "$lock_timeout" -ge 0 &&
       test $((lock_now - lock_started)) -ge "$lock_timeout"; then
      echo "ARM64e trace-PAUTH contract lock timed out: $lock_dir" >&2
      if test -f "$lock_dir/owner"; then
        echo "owner:" >&2
        sed -n '1,20p' "$lock_dir/owner" >&2 || true
      fi
      exit 2
    fi
    if test "$lock_announced" = 0; then
      echo "waiting for Lua test runner lock: $lock_dir" >&2
      lock_announced=1
    fi
    sleep 0.2
  done

  lock_held=1
  {
    printf 'time=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'cmd=%s\n' "$0"
  } >"$lock_dir/owner" 2>/dev/null || true
}

acquire_lock
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64e-trace-pauth.XXXXXX")

archive=$root/src/libluajit.a
vm_object=$root/src/lj_vm.o
fixture=$tmpdir/t-arm64e-jit-trace-pauth
macros=$tmpdir/macros-arm64e.txt
vm_disasm=$tmpdir/vm-arm64e.disasm
jloop_disasm=$tmpdir/vm-arm64e-jloop.disasm
jforl_disasm=$tmpdir/vm-arm64e-jforl.disasm

restore_needed=1
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean \
    TARGET_FLAGS='-arch arm64e -mbranch-protection=bti' \
    XCFLAGS="$pauth_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" \
    TARGET_FLAGS='-arch arm64e -mbranch-protection=bti' \
    XCFLAGS="$pauth_xcflags"

# shellcheck disable=SC2086 # pauth_xcflags intentionally expands.
"$cc" -arch arm64e -mbranch-protection=bti \
  -mmacosx-version-min="$minver" $pauth_xcflags -I"$root/src" \
  -dM -E -x c -include lj_arch.h /dev/null >"$macros"
for setting in \
  'LJ_ABI_PAUTH 1' \
  'LJ_ABI_BRANCH_TRACK 1' \
  'LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_FORL_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_FUNCF_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_FORL_NATIVE_ENTRY_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED 1'; do
  grep -F "#define $setting" "$macros" >/dev/null || {
    echo "ARM64e trace-PAUTH gate mismatch: $setting" >&2
    exit 1
  }
done

# shellcheck disable=SC2086 # pauth_xcflags intentionally expands.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64e \
  -mbranch-protection=bti -mmacosx-version-min="$minver" \
  $pauth_xcflags -I"$root/src" \
  "$root/tests/t-arm64e-jit-trace-pauth.c" "$archive" -lm -pthread \
  -o "$fixture"
otool -hv "$fixture" | grep -E 'ARM64[[:space:]]+E' >/dev/null

# Pin both the typed atomic access and the actual authenticated VM transfer.
grep -A4 '^static LJ_AINLINE ASMFunction trace_mcauth_acq' \
  "$root/src/lj_jit.h" | grep -F 'la_loadfunc_acq(&T->mcauth)' >/dev/null
otool -tvV "$vm_object" >"$vm_disasm"
awk '/^_lj_BC_JLOOP:/ { copy=1 }
     copy { print }
     copy && /^_lj_BC_JMP:/ { exit }' \
  "$vm_disasm" >"$jloop_disasm"
test -s "$jloop_disasm"
grep -E 'bti[[:space:]]+j' "$jloop_disasm" >/dev/null
grep -E 'braa[[:space:]]+x1, x0' "$jloop_disasm" >/dev/null
awk '/^_lj_BC_JFORL:/ { copy=1 }
     copy { print }
     copy && /^_lj_BC_ITERL:/ { exit }' \
  "$vm_disasm" >"$jforl_disasm"
test -s "$jforl_disasm"
grep -E 'bti[[:space:]]+j' "$jforl_disasm" >/dev/null
grep -E 'bl[[:space:]]+0x[0-9a-f]+' "$jforl_disasm" >/dev/null
grep -E 'sub[[:space:]]+sp, sp, #0x10' "$jforl_disasm" >/dev/null
grep -E 'braa[[:space:]]+x1, x0' "$jforl_disasm" >/dev/null

fixture_source=$root/tests/t-arm64e-jit-trace-pauth.c
for required in \
  'expect_exact_body(J, T, pt, site);' \
  'expect_exact_forl_body(J, T, pt);' \
  'expect_valid_trace_signature(T);' \
  'TRACE_ARM64_INT_LOOP_ADMITTED' \
  'TRACE_ARM64_INT_FORL_ADMITTED' \
  'live == BCINS_AD(BC_JFORL, bc_a(startins), 1)' \
  'trace_mcode_acq(T)[0] == A64I_BTI_J' \
  'la_storefunc_rel(&T->mcauth, injected)' \
  'ptrauth_strip(valid, ptrauth_key_function_pointer)' \
  'ptrauth_sign_unauthenticated(raw,' \
  'ptrauth_key_function_pointer, 0)' \
  'ptrauth_key_function_pointer, wrong_discriminator)' \
  'signal_negative_ready();' \
  'posix_spawn_file_actions_adddup2(&actions, ready_pipe[1], 3)' \
  'WIFSIGNALED(status)' \
  'WTERMSIG(status) != SIGBUS' \
  'WEXITSTATUS(status) != 0' \
  'la_storefunc_rel(&T->mcauth, original)' \
  'spawn_mode(self, "jloop-raw", 1)' \
  'spawn_mode(self, "jloop-ia-zero", 1)' \
  'spawn_mode(self, "jloop-wrong-trace", 1)' \
  'spawn_mode(self, "jforl-control", 0)' \
  'spawn_mode(self, "jforl-raw", 1)' \
  'spawn_mode(self, "jforl-ia-zero", 1)' \
  'spawn_mode(self, "jforl-wrong-trace", 1)'; do
  grep -F "$required" "$fixture_source" >/dev/null || {
    echo "ARM64e trace-PAUTH fixture lost required proof: $required" >&2
    exit 1
  }
done

# The supervisor uses posix_spawn + waitpid, rather than shell exit-code
# conventions, so SIGBUS cannot be confused with exit(138) or SIGABRT.
ulimit -c 0 2>/dev/null || true
"$fixture" supervise

echo "arm64e_jit_trace_pauth_contract OK: valid LOOP/FORL traces entered; raw, IA/0, and wrong-trace signatures faulted at authenticated JLOOP and JFORL entry"
