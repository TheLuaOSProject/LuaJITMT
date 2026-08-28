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
jfuncf_disasm=$tmpdir/vm-arm64e-jfuncf.disasm
jfuncf_source=$tmpdir/vm-arm64e-jfuncf.dasc

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
  'LJ_TARGET_ARM64 1' \
  'LJ_ABI_PAUTH 1' \
  'LJ_ABI_BRANCH_TRACK 1' \
  'LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_FORL_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_FUNCF_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_FIRST_SIDE_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_FORL_NATIVE_ENTRY_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED 1'; do
  grep -F "#define $setting" "$macros" >/dev/null || {
    echo "ARM64e trace-PAUTH gate mismatch: $setting" >&2
    exit 1
  }
done
grep -E '^#define TRACE_ARM64_TRUE_FUNCF_ADMITTED[[:space:]]+0x40$' \
  "$root/src/lj_jit.h" >/dev/null

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
awk '/^_lj_BC_JFUNCF:/ { copy=1 }
     copy { print }
     copy && /^_lj_BC_FUNCV:/ { exit }' \
  "$vm_disasm" >"$jfuncf_disasm"
test -s "$jfuncf_disasm"
grep -E 'bti[[:space:]]+j' "$jfuncf_disasm" >/dev/null
grep -E 'bl[[:space:]]+0x[0-9a-f]+' "$jfuncf_disasm" >/dev/null
grep -E 'cbz[[:space:]]+x0,' "$jfuncf_disasm" >/dev/null
grep -E 'cbz[[:space:]]+x1,' "$jfuncf_disasm" >/dev/null
grep -E 'sub[[:space:]]+sp, sp, #0x10' "$jfuncf_disasm" >/dev/null
grep -E 'braa[[:space:]]+x1, x0$' "$jfuncf_disasm" >/dev/null
if grep -E 'br[[:space:]]+x1$|braaz[[:space:]]+x1$' \
     "$jfuncf_disasm" >/dev/null; then
  echo "ARM64e JFUNCF lost its trace-discriminated transfer" >&2
  exit 1
fi
if grep -E 'stlr[[:space:]]+xzr, \[x14\]' \
     "$jfuncf_disasm" >/dev/null; then
  echo "open ARM64e JFUNCF unexpectedly clears its successful helper lease" >&2
  exit 1
fi
jfuncf_call=$(grep -nE 'bl[[:space:]]+0x[0-9a-f]+' \
  "$jfuncf_disasm" | sed -n '1p' | cut -d: -f1)
jfuncf_trace=$(grep -nE 'cbz[[:space:]]+x0,' \
  "$jfuncf_disasm" | sed -n '1p' | cut -d: -f1)
jfuncf_target=$(grep -nE 'cbz[[:space:]]+x1,' \
  "$jfuncf_disasm" | sed -n '1p' | cut -d: -f1)
jfuncf_sub=$(grep -nE 'sub[[:space:]]+sp, sp, #0x10' \
  "$jfuncf_disasm" | sed -n '1p' | cut -d: -f1)
jfuncf_branch=$(grep -nE 'braa[[:space:]]+x1, x0$' \
  "$jfuncf_disasm" | sed -n '1p' | cut -d: -f1)
test -n "$jfuncf_call" && test -n "$jfuncf_trace" &&
test -n "$jfuncf_target" && test -n "$jfuncf_sub" &&
test -n "$jfuncf_branch" &&
test "$jfuncf_call" -lt "$jfuncf_trace" &&
test "$jfuncf_trace" -lt "$jfuncf_target" &&
test "$jfuncf_target" -lt "$jfuncf_sub" &&
test "$jfuncf_sub" -lt "$jfuncf_branch"

awk '/case BC_JFUNCF:/ { seen++; if (seen == 2) copy=1 }
     copy { print }
     copy && /case BC_JFUNCV:/ { exit }' \
  "$root/src/vm_arm64.dasc" >"$jfuncf_source"
test -s "$jfuncf_source"
for required in \
  'bl extern lj_trace_enter_root' \
  'cbz CRET1, >4' \
  'cbz CARG2, >4' \
  '#if LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED' \
  'sub sp, sp, #16' \
  'br_trace_auth CARG2, CRET1'; do
  grep -F "$required" "$jfuncf_source" >/dev/null || {
    echo "ARM64e JFUNCF VM source lost required proof: $required" >&2
    exit 1
  }
done
jfuncf_source_call=$(grep -nF 'bl extern lj_trace_enter_root' \
  "$jfuncf_source" | sed -n '1p' | cut -d: -f1)
jfuncf_source_sub=$(grep -nF 'sub sp, sp, #16' \
  "$jfuncf_source" | sed -n '1p' | cut -d: -f1)
jfuncf_source_branch=$(grep -nF 'br_trace_auth CARG2, CRET1' \
  "$jfuncf_source" | sed -n '1p' | cut -d: -f1)
test "$jfuncf_source_call" -lt "$jfuncf_source_sub" &&
test "$jfuncf_source_sub" -lt "$jfuncf_source_branch"

fixture_source=$root/tests/t-arm64e-jit-trace-pauth.c
for required in \
  'expect_exact_body(J, T, pt, site);' \
  'expect_exact_forl_body(J, T, pt);' \
  'expect_exact_funcf_body(J, T, pt);' \
  'expect_valid_trace_signature(T);' \
  'ENTRY_SITE_JFUNCF' \
  'function __arm64e_pauth_funcf_true(a, b) return true end ' \
  'TRACE_ARM64_INT_LOOP_ADMITTED' \
  'TRACE_ARM64_INT_FORL_ADMITTED' \
  'TRACE_ARM64_TRUE_FUNCF_ADMITTED' \
  'live == BCINS_AD(BC_JFORL, bc_a(startins), 1)' \
  'live == BCINS_AD(BC_JFUNCF, bc_a(startins), 1)' \
  'kpri == BCINS_AD(BC_KPRI, result, 2)' \
  'ret == BCINS_AD(BC_RET1, result, 2)' \
  'trace_link_acq(T) == 0' \
  'trace_linktype_acq(T) == LJ_TRLINK_RETURN' \
  'expect_ir(ir, FUNCF_R_XPOLL, IR_XPOLL' \
  'trace_nsnap_acq(T) == 2' \
  'SNAP(result_slot, 0, REF_TRUE)' \
  'trace_mcloop_acq(T) == 0' \
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
  'spawn_mode(self, "jforl-wrong-trace", 1)' \
  'spawn_mode(self, "jfuncf-control", 0)' \
  'spawn_mode(self, "jfuncf-raw", 1)' \
  'spawn_mode(self, "jfuncf-ia-zero", 1)' \
  'spawn_mode(self, "jfuncf-wrong-trace", 1)'; do
  grep -F "$required" "$fixture_source" >/dev/null || {
    echo "ARM64e trace-PAUTH fixture lost required proof: $required" >&2
    exit 1
  }
done

# The child publishes its readiness byte after signature mutation and full
# semantic revalidation, immediately before the potentially faulting Lua call.
ready_line=$(grep -nF 'signal_negative_ready();' "$fixture_source" |
  sed -n '1p' | cut -d: -f1)
pcall_line=$(grep -nF 'status = lua_pcall(L, nargs, 1, 0);' \
  "$fixture_source" | sed -n '1p' | cut -d: -f1)
test -n "$ready_line" && test -n "$pcall_line" &&
test "$ready_line" -lt "$pcall_line"

# The supervisor uses posix_spawn + waitpid, rather than shell exit-code
# conventions, so SIGBUS cannot be confused with exit(138) or SIGABRT.
ulimit -c 0 2>/dev/null || true
"$fixture" supervise

echo "arm64e_jit_trace_pauth_contract OK: valid LOOP/FORL/JFUNCF traces entered; raw, IA/0, and wrong-trace signatures faulted at authenticated JLOOP, JFORL, and JFUNCF entry"
