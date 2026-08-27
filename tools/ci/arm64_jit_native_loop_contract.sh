#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_native_loop_contract SKIP: requires native macOS arm64"
  exit 0
fi

lock_dir=$root/src/.lj-test-run.lock
lock_held=0
restore_needed=0
tmpdir=
cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if test "$restore_needed" = 1; then
    env MACOSX_DEPLOYMENT_TARGET="$minver" \
      make -C "$root/src" clean TARGET_FLAGS='-arch arm64' \
        XCFLAGS="$xcflags" >/dev/null 2>&1 || status=1
    env MACOSX_DEPLOYMENT_TARGET="$minver" \
      make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
        XCFLAGS="$xcflags" >/dev/null 2>&1 || status=1
  fi
  if test "$lock_held" = 1; then
    rm -f "$lock_dir/owner"
    rmdir "$lock_dir" 2>/dev/null || true
  fi
  if test -n "$tmpdir"; then
    rm -rf "$tmpdir"
  fi
  exit "$status"
}

trap cleanup EXIT HUP INT TERM

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
      echo "ARM64 native-loop contract lock timed out: $lock_dir" >&2
      if test -f "$lock_dir/owner"; then
        echo "owner:" >&2
        cat "$lock_dir/owner" >&2 || true
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
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-native-loop.XXXXXX")

jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
cc=${CC:-clang}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS -DLUAJIT_MCODE_TEST'
pauth_xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR"
archive=$root/src/libluajit.a
vm_object=$root/src/lj_vm.o
fixture=$tmpdir/t-arm64-jit-native-loop
fixture_obj=$tmpdir/t-arm64-jit-native-loop.o
macros=$tmpdir/macros.txt
entry_helper=$tmpdir/root-entry-helper.c
pauth_fixture=$tmpdir/t-arm64-jit-native-loop-arm64e
pauth_macros=$tmpdir/macros-arm64e.txt
jloop_source=$tmpdir/vm-jloop.dasc
jloop_open=$tmpdir/vm-jloop-open.dasc
vm_disasm=$tmpdir/vm.disasm
jloop_disasm=$tmpdir/vm-jloop.disasm
pauth_vm_disasm=$tmpdir/vm-arm64e.disasm
pauth_jloop_disasm=$tmpdir/vm-arm64e-jloop.disasm
restore_needed=1

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" XCFLAGS="$xcflags" clean
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" XCFLAGS="$xcflags"

test "$(lipo -archs "$archive")" = arm64
nm "$archive" | grep ' T _lj_trace_test_root_entry_reset$' >/dev/null
nm "$archive" | grep ' T _lj_trace_test_reset_exit_stats$' >/dev/null

# Evaluate the production architecture policy, rather than allowing command
# line overrides to make a closed build look open to the fixture.
# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -arch arm64 -mmacosx-version-min="$minver" $xcflags \
  -I"$root/src" -dM -E -x c -include lj_arch.h /dev/null >"$macros"
for setting in \
  'LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_FUNCF_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_EXIT_TARGET_SLOTS 1' \
  'LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED 1'; do
  grep -F "#define $setting" "$macros" >/dev/null || {
    echo "ARM64 native-loop gate mismatch: $setting" >&2
    exit 1
  }
done

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  -c "$root/tests/t-arm64-jit-native-loop.c" -o "$fixture_obj"
# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  "$root/tests/t-arm64-jit-native-loop.c" "$archive" -lm -pthread \
  -o "$fixture"
"$fixture" direct

# A one-PHI loop produces exactly one allocator RENAME. This is distinct from
# both the exact two-RENAME C fixture and the spare-NOP suffix: semantic+1 must
# fall through to RENAME validation when that sole instruction is not a NOP.
env LUA_PATH="$root/src/?.lua;$root/src/jit/?.lua;;" "$root/src/luajit" -e '
  local util = require("jit.util")
  local vmdef = require("jit.vmdef")
  local function opname(ot)
    local op = math.floor(ot / 256)
    return vmdef.irnames:sub(op * 6 + 1, op * 6 + 6)
  end
  jit.flush(); jit.on()
  jit.opt.start("hotloop=1", "hotexit=1", "maxtrace=2")
  function __arm64_one_rename(n)
    local i = 0
    while i < n do i = i + 1 end
    return i
  end
  assert(__arm64_one_rename(20) == 20)
  assert(__arm64_one_rename(20) == 20)
  assert(__arm64_one_rename(20) == 20)
  assert(__arm64_one_rename(20) == 20)
  assert(__arm64_one_rename(20) == 20)
  assert(util.traceinfo(1), "one-RENAME loop did not record")
  assert(util.traceinfo(2) == nil, "one-RENAME loop published a side trace")
  local renames = 0
  for ref = 1, 256 do
    local _, ot = util.traceir(1, ref)
    if not ot then break end
    if opname(ot) == "RENAME" then renames = renames + 1 end
  end
  assert(renames == 1, "expected exactly one allocator RENAME")
'

# The successful JLOOP path must reserve the fixed interpreter spill area
# before the authenticated trace branch. The generated ordinary-arm64 VM uses
# BR here; br_trace_auth becomes BRAA with the same ordering on arm64e.
awk '/case BC_JLOOP:/ { seen++; if (seen == 2) copy=1 }
     copy { print }
     copy && /case BC_JMP:/ { exit }' \
  "$root/src/vm_arm64.dasc" >"$jloop_source"
test -s "$jloop_source"
awk '/#if LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED/ { gate=1 }
     gate && /^#else/ { open=1; next }
     open && /^#endif/ { exit }
     open { print }' "$jloop_source" >"$jloop_open"
test "$(grep -Fc 'sub sp, sp, #16' "$jloop_open")" = 1
test "$(grep -Fc 'br_trace_auth CARG2, CRET1' "$jloop_open")" = 1
sub_line=$(grep -nF 'sub sp, sp, #16' "$jloop_open" | cut -d: -f1)
branch_line=$(grep -nF 'br_trace_auth CARG2, CRET1' "$jloop_open" | \
  cut -d: -f1)
test "$sub_line" -lt "$branch_line"

otool -tvV "$vm_object" >"$vm_disasm"
awk '/^_lj_BC_JLOOP:/ { copy=1 }
     copy { print }
     copy && /^_lj_BC_JMP:/ { exit }' "$vm_disasm" >"$jloop_disasm"
test -s "$jloop_disasm"
grep -E 'sub[[:space:]]+sp, sp, #0x10' "$jloop_disasm" >/dev/null
grep -E 'br[[:space:]]+x1' "$jloop_disasm" >/dev/null
sub_line=$(grep -nE 'sub[[:space:]]+sp, sp, #0x10' "$jloop_disasm" | \
  sed -n '1p' | cut -d: -f1)
branch_line=$(grep -nE 'br[[:space:]]+x1' "$jloop_disasm" | \
  sed -n '1p' | cut -d: -f1)
test "$sub_line" -lt "$branch_line"

# Pin each granular surface gate. The compatibility summaries are not a
# behavioral predicate for this integer-loop execution contract.
grep -A40 '^void LJ_FASTCALL lj_trace_hot' "$root/src/lj_trace.c" | \
  grep -F '#if LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED' >/dev/null
grep -A20 '^void lj_trace_ins' "$root/src/lj_trace.c" | \
  grep -F '#if LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED' >/dev/null
grep -A30 '^static void trace_hotside' "$root/src/lj_trace.c" | \
  grep -F '#if LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED' >/dev/null
grep -A20 '^void LJ_FASTCALL lj_trace_stitch' "$root/src/lj_trace.c" | \
  grep -F '#if LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED' >/dev/null
grep -F '#if LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED' \
  "$jloop_source" >/dev/null

# The post-admission test pause is deliberately after the helper's final
# request/bytecode recheck. A publisher released from this stage can therefore
# be observed only by native XPOLL (or a later VM poll), never by admission.
awk '/^lj_trace_enter_root\(/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$root/src/lj_trace.c" >"$entry_helper"
test -s "$entry_helper"
final_pending=$(grep -n 'trace_root_entry_request_pending(tg)' \
  "$entry_helper" | sed -n '3p' | cut -d: -f1)
postadmission=$(grep -n 'LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION' \
  "$entry_helper" | sed -n '1p' | cut -d: -f1)
tmpbuf=$(grep -n 'setsbufL(&tg->tmpbuf, L)' "$entry_helper" | \
  sed -n '1p' | cut -d: -f1)
test -n "$final_pending" && test -n "$postadmission" && test -n "$tmpbuf"
test "$final_pending" -lt "$postadmission"
test "$postadmission" -lt "$tmpbuf"
test "$(grep -Fc 'trace_root_entry_request_pending(tg)' \
  "$entry_helper")" = 3

fixture_source=$root/tests/t-arm64-jit-native-loop.c
for required in \
  "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2')" \
  'while i<n do i=i+1 x=x+i end' \
  'LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION' \
  'POSTADMISSION_PROFILE' \
  'POSTADMISSION_STOPREQ' \
  'lj_tg_profile_request_rel(tg, 1)' \
  'gc2_hs_actions_rel(g, LJ_GC2_HS_STOPREQ)' \
  'gc2_hs_pending_rel(g, 1)' \
  'gc2_hs_epoch_rel(g, publisher->epoch + 1u)' \
  'lj_tg_reqmask_rel(tg, LJ_GC2_HS_STOPREQ)' \
  'lj_tg_poll_rel(tg, 1)' \
  'expect_single_exit(5)' \
  'expect_profile_exit_and_reentry()' \
  'expect_single_exit(8)' \
  'thread interrupted: VM shutdown' \
  'lj_trace_test_root_entry_publishes() == 1' \
  'lj_trace_test_root_entry_cleanups() == 0' \
  'lj_trace_test_exit_calls() == 1' \
  'lj_trace_test_exit_calls() == 2' \
  'lj_trace_test_first_exitno() == 5' \
  'lj_trace_test_last_exitno() == exitno' \
  'lj_tg_hs_epoch_ack_acq(tg) == epoch + 1u' \
  'gc2_hs_pending_acq(g) == 0' \
  'lj_tg_profile_request_acq(tg) == 0' \
  '(TGF_STOPREQ|TGF_STOPREQ_FRESH)' \
  'TRACE_ARM64_INT_LOOP_ADMITTED' \
  'trace_spadjust_acq(T) == 0' \
  'trace_topslot_acq(T) == (MSize)pt->framesize' \
  '168 + (LJ_ABI_BRANCH_TRACK ? sizeof(MCode) : 0)' \
  'trace_mcode_acq(T)[0] == A64I_BTI_J' \
  'trace_exittab_acq(T)' \
  'exitstub_trace_fallback_addr_(gates)' \
  'trace_exittarget_arm64_acq(T, i) == fallback' \
  'A64I_LDARx' \
  'A64I_BR_G_AUTH' \
  'run_lua(L, "jit.flush()")' \
  'proto_trace_acq(pt) == 0' \
  'trace_mcloop_acq(T) & (sizeof(MCode)-1u)' \
  'bc_op(back) == BC_JMP' \
  'bc_j(back) < 0' \
  'assert(!ra_hasspill(ir[ref].s))' \
  'R_RENAME_I' 'R_RENAME_X' \
  'assert(L->cframe == saved_cframe)'; do
  grep -F "$required" "$fixture_source" >/dev/null || {
    echo "ARM64 native-loop fixture lost required proof: $required" >&2
    exit 1
  }
done
if grep -E 'LJ_ARM64_JIT_(RECORDER_ADMISSION|NATIVE_ENTRY)_FAIL_CLOSED|LJ_ARM64_JIT_FAIL_CLOSED' \
     "$fixture_source" >/dev/null; then
  echo "ARM64 native-loop fixture uses a compatibility summary gate" >&2
  exit 1
fi
test "$(grep -Fc 'call_sum_and_check_cframe(L, 20, 210)' \
  "$fixture_source")" = 3 || {
  echo "ARM64 native-loop fixture must check record, profile and recovery calls" >&2
  exit 1
}

# Execute the identical strict root under the authenticated ABI. The fixed
# fallback is placement-independent and always uses the K64 load plus BLRAAZ;
# normal and randomized mcode hints must execute the same immutable gates.
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
  -dM -E -x c -include lj_arch.h /dev/null >"$pauth_macros"
for setting in \
  'LJ_ABI_PAUTH 1' \
  'LJ_ABI_BRANCH_TRACK 1' \
  'LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_FUNCF_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_EXIT_TARGET_SLOTS 1' \
  'LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED 1'; do
  grep -F "#define $setting" "$pauth_macros" >/dev/null || {
    echo "ARM64e native-loop gate mismatch: $setting" >&2
    exit 1
  }
done

# shellcheck disable=SC2086 # pauth_xcflags intentionally expands.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64e \
  -mbranch-protection=bti -mmacosx-version-min="$minver" \
  $pauth_xcflags -I"$root/src" \
  "$root/tests/t-arm64-jit-native-loop.c" "$archive" -lm -pthread \
  -o "$pauth_fixture"
otool -hv "$pauth_fixture" | grep -E 'ARM64[[:space:]]+E' >/dev/null
"$pauth_fixture" direct
LUAJIT_MCODE_TEST=R "$pauth_fixture" randomized

otool -tvV "$vm_object" >"$pauth_vm_disasm"
awk '/^_lj_BC_JLOOP:/ { copy=1 }
     copy { print }
     copy && /^_lj_BC_JMP:/ { exit }' \
  "$pauth_vm_disasm" >"$pauth_jloop_disasm"
test -s "$pauth_jloop_disasm"
grep -E 'bti[[:space:]]+j' "$pauth_jloop_disasm" >/dev/null
grep -E 'sub[[:space:]]+sp, sp, #0x10' \
  "$pauth_jloop_disasm" >/dev/null
grep -E 'braa[[:space:]]+x1, x0' "$pauth_jloop_disasm" >/dev/null
sub_line=$(grep -nE 'sub[[:space:]]+sp, sp, #0x10' \
  "$pauth_jloop_disasm" | sed -n '1p' | cut -d: -f1)
branch_line=$(grep -nE 'braa[[:space:]]+x1, x0' \
  "$pauth_jloop_disasm" | sed -n '1p' | cut -d: -f1)
test "$sub_line" -lt "$branch_line"

# Leave the shared checkout in ordinary native experimental mode.
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' XCFLAGS="$xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" \
    TARGET_FLAGS='-arch arm64' XCFLAGS="$xcflags"
restore_needed=0

echo "arm64_jit_native_loop_contract OK: strict ARM64 and ARM64e/BTI BC_LOOP executed XPOLL lifecycle through placement-invariant authenticated exit tables"
