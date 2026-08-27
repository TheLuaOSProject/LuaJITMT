#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_native_forl_contract SKIP: requires native macOS arm64"
  exit 0
fi

if test -z "${SDKROOT:-}"; then
  SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
  export SDKROOT
fi

jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
cc=${CC:-$(xcrun --sdk macosx --find clang)}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS -DLUAJIT_MCODE_TEST'
pauth_xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR"
archive=$root/src/libluajit.a
vm_object=$root/src/lj_vm.o
fixture_source=$root/tests/t-arm64-jit-native-forl.c
trace_source=$root/src/lj_trace.c
vm_source=$root/src/vm_arm64.dasc
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
       test $((lock_now-lock_started)) -ge "$lock_timeout"; then
      echo "ARM64 native-FORL contract lock timed out: $lock_dir" >&2
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
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-native-forl.XXXXXX")
fixture=$tmpdir/t-arm64-jit-native-forl
fixture_obj=$tmpdir/t-arm64-jit-native-forl.o
pauth_fixture=$tmpdir/t-arm64-jit-native-forl-arm64e
pauth_obj=$tmpdir/t-arm64-jit-native-forl-arm64e.o
macros=$tmpdir/macros.txt
pauth_macros=$tmpdir/macros-arm64e.txt
root_helpers=$tmpdir/root-helpers.c
entry_helper=$tmpdir/root-entry.c
forl_vm=$tmpdir/forl-vm.dasc
fp_vm=$tmpdir/forl-fp.dasc
vm_disasm=$tmpdir/vm.disasm
forl_disasm=$tmpdir/forl.disasm
pauth_vm_disasm=$tmpdir/vm-arm64e.disasm
pauth_forl_disasm=$tmpdir/forl-arm64e.disasm
restore_needed=1

# The executable fixture separates the four semantic boundaries: taken
# integer entry, false integer edge, FP branch-only recovery and post-update
# overflow recovery. It also races an A-only bytecode generation change after
# both metadata views, then flushes and reuses trace slot 1.
for required in \
  'LJ_ARM64_JIT_FORL_NATIVE_ENTRY_FAIL_CLOSED' \
  'TRACE_ARM64_INT_FORL_ADMITTED' \
  'live == BCINS_AD(BC_JFORL, bc_a(startins), 1)' \
  'lj_trace_test_root_entry_publishes() == 1' \
  'lj_trace_test_root_entry_cleanups() == 0' \
  'lj_trace_test_first_exitno() == 5' \
  'call_forl_integer(L, 1, 9) == 10' \
  'call_forl_number_stop(L, 3.5, 0) == 6.0' \
  'lj_trace_test_root_entry_startins_calls() == 2' \
  'test_vm_branch_only_rejection(L)' \
  'publish_profile_rejection' \
  'call_forl_integer(L, 2, 0) == 3' \
  'lj_trace_test_root_entry_cleanups() == 1' \
  'lj_tg_profile_request_acq(tg) == 0' \
  'call_forl_integer(L, 3, 2147483646) == 2147483652.0' \
  'lj_trace_test_root_entry_publishes() == 2' \
  'lj_trace_test_first_exitno() == 1' \
  'lj_trace_test_last_exitno() == 0' \
  'test_signed_step_edges(0)' \
  'test_signed_step_edges(1)' \
  'call_edge(L, name, 10, 1) == 10' \
  'call_edge(L, name, (lua_Integer)INT32_MAX,' \
  'call_edge(L, name, (lua_Integer)INT32_MIN,' \
  'LJ_TRACE_ROOT_ENTRY_PAUSE_POSTMETADATA' \
  'race.replacement = BCINS_AD(BC_JFORL, wronga, 1)' \
  'lj_trace_enter_root(J, pc, 1, L, L->base, live)' \
  'lj_trace_test_root_entry_cleanups() == 1' \
  'run_lua(L, "jit.flush()")' \
  'trace_startpc_acq(T) == firstpc' \
  'trace_startins_acq(T) == firstins'; do
  grep -F "$required" "$fixture_source" >/dev/null || {
    echo "ARM64 native-FORL fixture lost proof: $required" >&2
    exit 1
  }
done

# Pin the C boundary to the consumed full instruction. A/D/op must match the
# immutable startins-derived generation and the current acquire-loaded word on
# all three metadata/final rechecks.
awk '/^static LJ_AINLINE int trace_root_entry_source_valid/ { copy=1 }
     copy { print }
     copy && /^static LJ_AINLINE GCtrace \*trace_root_entry_slot_acq/ { exit }' \
  "$trace_source" >"$root_helpers"
for required in \
  'sourceop == (uint32_t)BC_JFORL' \
  '!LJ_ARM64_JIT_FORL_NATIVE_ENTRY_FAIL_CLOSED' \
  'return op == BC_FORL'; do
  grep -F "$required" "$root_helpers" >/dev/null || {
    echo "ARM64 native-FORL source gate changed: $required" >&2
    exit 1
  }
done

awk '/^lj_trace_enter_root\(/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$trace_source" >"$entry_helper"
for required in \
  'BCIns sourceins' \
  'uint32_t sourceop = (uint32_t)bc_op(sourceins);' \
  '(TraceNo)bc_d(sourceins) != traceno' \
  'BCIns expected = BCINS_AD((BCOp)sourceop, bc_a(startins), traceno);' \
  'if (sourceins != expected || ins != sourceins)' \
  '(sourceop == (uint32_t)BC_JFUNCF ?' \
  'TRACE_ARM64_TRUE_FUNCF_ADMITTED : TRACE_ARM64_INT_LOOP_ADMITTED)' \
  '(v->admission & root_admission) == expected_admission' \
  'trace_root_entry_loop_geometry(sourceop, pt, pc, v->startins)' \
  'bc_op(startins) != BC_FORL' \
  'bc_op(fori) == BC_FORI' \
  'bc_a(fori) == ra' \
  'bc_j(fori) > 0' \
  'endpos == (int64_t)pos+1' \
  'target = (ASMFunction)(void *)mcode;' \
  'LJ_TRACE_ROOT_ENTRY_PAUSE_POSTMETADATA'; do
  grep -F "$required" "$trace_source" >/dev/null || {
    echo "ARM64 native-FORL entry proof changed: $required" >&2
    exit 1
  }
done
test "$(grep -Fc 'trace_root_entry_bytecode_valid(pc, pt, traceno, sourceop,' \
  "$entry_helper")" = 3

# The integer JFORL helper is reachable only through the taken comparison after
# both rooted stores. It passes the consumed full word, uses T->mcode, reserves
# SPS_FIXED and authenticates the branch. Rejection restores that consumed word
# before the existing branch-only tail, so the increment cannot run twice.
awk '/^  case BC_JFORI:/ { seen++; if (seen == 2) copy=1 }
     copy { print }
     copy && /^  case BC_ITERL:/ { exit }' "$vm_source" >"$forl_vm"
for required in \
  'adds CARG1w, CARG1w, CARG3w' \
  'bvs >2' \
  'str TMP0, FOR_IDX' \
  'str TMP0, FOR_EXT' \
  'ble >9' \
  '|9:  // Taken integer edge: IDX/EXT are authoritative before admission.' \
  'mov RAw, INSw' \
  'mov CARG6w, INSw' \
  'bl extern lj_trace_enter_root' \
  'sub sp, sp, #16' \
  'br_trace_auth CARG2, CRET1' \
  'mov INSw, RAw' \
  'b =>BC_JLOOP'; do
  grep -F "$required" "$forl_vm" >/dev/null || {
    echo "ARM64 native-FORL VM path changed: $required" >&2
    exit 1
  }
done
adds_line=$(grep -nF 'adds CARG1w, CARG1w, CARG3w' "$forl_vm" | \
  sed -n '1p' | cut -d: -f1)
bvs_line=$(grep -nF 'bvs >2' "$forl_vm" | sed -n '1p' | cut -d: -f1)
idx_line=$(grep -nF 'str TMP0, FOR_IDX' "$forl_vm" | \
  sed -n '1p' | cut -d: -f1)
ext_line=$(grep -nF 'str TMP0, FOR_EXT' "$forl_vm" | \
  sed -n '1p' | cut -d: -f1)
taken_line=$(grep -nF 'ble >9' "$forl_vm" | sed -n '1p' | cut -d: -f1)
entry_line=$(grep -nF 'bl extern lj_trace_enter_root' "$forl_vm" | \
  sed -n '1p' | cut -d: -f1)
reserve_line=$(grep -nF 'sub sp, sp, #16' "$forl_vm" | \
  sed -n '1p' | cut -d: -f1)
branch_line=$(grep -nF 'br_trace_auth CARG2, CRET1' "$forl_vm" | \
  sed -n '1p' | cut -d: -f1)
test "$adds_line" -lt "$bvs_line" && test "$bvs_line" -lt "$idx_line" &&
test "$idx_line" -lt "$ext_line" &&
test "$ext_line" -lt "$taken_line" && test "$taken_line" -lt "$entry_line" &&
test "$entry_line" -lt "$reserve_line" && test "$reserve_line" -lt "$branch_line"
test "$(grep -Fc 'bl extern lj_trace_enter_root' "$forl_vm")" = 1

# Both FP JFORL signs still tail straight to the branch-only recovery label.
# No helper, native frame or authenticated branch may appear in that FP slice.
awk '/\|5:  \/\/ FP loop\./ { copy=1 }
     copy && /^    if \(op == BC_JFORL\) \{/ { exit }
     copy { print }' "$forl_vm" >"$fp_vm"
test "$(grep -Fc 'bls =>BC_JLOOP' "$fp_vm")" = 2
if grep -E 'lj_trace_enter_root|sub sp, sp|br_trace_auth' "$fp_vm" >/dev/null; then
  echo "ARM64 FP JFORL path gained native entry" >&2
  exit 1
fi

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' XCFLAGS="$xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$xcflags"

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -arch arm64 -mmacosx-version-min="$minver" $xcflags \
  -I"$root/src" -dM -E -x c -include lj_arch.h /dev/null >"$macros"
for setting in \
  'LJ_TARGET_ARM64 1' \
  'LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_FORL_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_FUNCF_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_FORL_NATIVE_ENTRY_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED 1'; do
  grep -E "^#define ${setting}$" "$macros" >/dev/null || {
    echo "ARM64 native-FORL gate mismatch: $setting" >&2
    exit 1
  }
done

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  -c "$fixture_source" -o "$fixture_obj"
"$cc" -arch arm64 -mmacosx-version-min="$minver" \
  "$fixture_obj" "$archive" -lm -pthread -o "$fixture"
ordinary_runs=${LJ_ARM64_NATIVE_FORL_RUNS:-3}
run=1
while test "$run" -le "$ordinary_runs"; do
  "$fixture"
  run=$((run+1))
done
LUAJIT_MCODE_TEST=R "$fixture"

otool -tvV "$vm_object" >"$vm_disasm"
awk '/^_lj_BC_JFORL:/ { copy=1 }
     copy { print }
     copy && /^_lj_BC_ITERL:/ { exit }' "$vm_disasm" >"$forl_disasm"
test -s "$forl_disasm"
grep -E 'bl[[:space:]]+0x[0-9a-f]+' "$forl_disasm" >/dev/null
grep -E 'sub[[:space:]]+sp, sp, #0x10' "$forl_disasm" >/dev/null
grep -E 'br[[:space:]]+x1' "$forl_disasm" >/dev/null

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
  'LJ_ARM64_JIT_FORL_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_FUNCF_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_FORL_NATIVE_ENTRY_FAIL_CLOSED 0'; do
  grep -E "^#define ${setting}$" "$pauth_macros" >/dev/null || {
    echo "ARM64e native-FORL gate mismatch: $setting" >&2
    exit 1
  }
done

# shellcheck disable=SC2086 # pauth_xcflags intentionally expands.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64e \
  -mbranch-protection=bti -mmacosx-version-min="$minver" \
  $pauth_xcflags -I"$root/src" -c "$fixture_source" -o "$pauth_obj"
"$cc" -arch arm64e -mbranch-protection=bti \
  -mmacosx-version-min="$minver" "$pauth_obj" "$archive" -lm -pthread \
  -o "$pauth_fixture"
otool -hv "$pauth_fixture" | grep -E 'ARM64[[:space:]]+E' >/dev/null
pauth_runs=${LJ_ARM64_NATIVE_FORL_PAUTH_RUNS:-2}
run=1
while test "$run" -le "$pauth_runs"; do
  "$pauth_fixture"
  run=$((run+1))
done
LUAJIT_MCODE_TEST=R "$pauth_fixture"

otool -tvV "$vm_object" >"$pauth_vm_disasm"
awk '/^_lj_BC_JFORL:/ { copy=1 }
     copy { print }
     copy && /^_lj_BC_ITERL:/ { exit }' \
  "$pauth_vm_disasm" >"$pauth_forl_disasm"
test -s "$pauth_forl_disasm"
grep -E 'bti[[:space:]]+j' "$pauth_forl_disasm" >/dev/null
grep -E 'sub[[:space:]]+sp, sp, #0x10' "$pauth_forl_disasm" >/dev/null
grep -E 'braa[[:space:]]+x1, x0' "$pauth_forl_disasm" >/dev/null

# Leave the checkout in its ordinary experimental configuration.
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' XCFLAGS="$xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$xcflags"
restore_needed=0

echo "arm64_jit_native_forl_contract OK: post-update integer JFORL native entry verified on ARM64/ARM64e; FP stayed branch-only"
