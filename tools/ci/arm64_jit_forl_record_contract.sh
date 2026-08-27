#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_forl_record_contract SKIP: requires native macOS arm64"
  exit 0
fi

jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
cc=${CC:-clang}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS -DLUAJIT_MCODE_TEST'
pauth_xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR"
archive=$root/src/libluajit.a
fixture_source=$root/tests/t-arm64-jit-forl-record.c
trace_source=$root/src/lj_trace.c
asm_source=$root/src/lj_asm.c
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
      echo "ARM64 FORL record contract lock timed out: $lock_dir" >&2
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
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-forl-record.XXXXXX")
fixture=$tmpdir/t-arm64-jit-forl-record
fixture_obj=$tmpdir/t-arm64-jit-forl-record.o
pauth_fixture=$tmpdir/t-arm64-jit-forl-record-arm64e
pauth_obj=$tmpdir/t-arm64-jit-forl-record-arm64e.o
macros=$tmpdir/macros.txt
pauth_macros=$tmpdir/macros-arm64e.txt
tuple_region=$tmpdir/tuple-region.txt
forl_shape=$tmpdir/forl-shape.txt
postra_region=$tmpdir/postra-region.txt
trace_stop=$tmpdir/trace-stop.txt
forl_vm=$tmpdir/forl-vm.txt
branch_only_vm=$tmpdir/branch-only-vm.txt
root_entry=$tmpdir/root-entry.txt
restore_needed=1

# Freeze the Stage 1 boundary in the executable fixture: publication is real,
# while every JFORL taken edge still recovers to the interpreter branch path.
for required in \
  'LJ_ARM64_JIT_FORL_RECORDER_FAIL_CLOSED' \
  'LJ_ARM64_JIT_FORL_NATIVE_ENTRY_FAIL_CLOSED' \
  'TRACE_ARM64_INT_FORL_ADMITTED' \
  'TRACE_ARM64_INT_LOOP_ADMITTED' \
  'proto_jit_startins_acq(pt, forlpc) == forl' \
  'memcpy(&pcbase, &map[positive_mapofs[sn]+positive_nent[sn]],' \
  'SNAP_NORESTORE, R_IDX' \
  'SNAP_NORESTORE, R_STOP' \
  'lj_trace_test_root_entry_publishes() == 0' \
  'lj_trace_test_root_entry_cleanups() == 0' \
  'lj_trace_test_exit_calls() == 0' \
  'lj_trace_test_root_entry_startins_calls() == 36' \
  'lj_trace_test_root_entry_startins_calls() == 13' \
  'callnum1(L, "__arm64_forl_positive", 3.5) == 6.0' \
  'lj_trace_test_root_entry_startins_calls() == 2' \
  'run_lua(L, "jit.flush()\n");' \
  'assert((BCIns)la_load32_acq((const uint32_t *)forlpc) == forl);' \
  'assert(trace_startpc_acq(T) == forlpc);' \
  'function __arm64_forl_negative_stop(n, stop)' \
  'bound = find_kint(T, INT32_MIN+3);' \
  'function __arm64_forl_zero(_)' \
  'assert(call1(L, "__arm64_forl_zero", 0) == 10);' \
  'assert(!trace_runnable_acq(traceref_safe(J, 1), 1));'; do
  grep -F "$required" "$fixture_source" >/dev/null || {
    echo "ARM64 FORL fixture lost Stage 1 proof: $required" >&2
    exit 1
  }
done

# The recorder must validate the exact FORI/FORL tuple on initial capture and
# on both START and RECORD ingress generations.
awk '/^static int trace_root_forl_tuple/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$trace_source" >"$tuple_region"
for required in \
  'bc_op(startins) != BC_FORL' \
  'trace_pc_in_proto_range(pc, bc, pt->sizebc)' \
  'bc_j(startins) >= 0' \
  '(MSize)ra+FORL_EXT >= (MSize)pt->framesize' \
  'bc_op(fori) != BC_FORI' \
  'bc_a(fori) != ra' \
  'bc_j(fori) <= 0' \
  'exitpos != (int64_t)pcpos+1' \
  'la_load32_acq((const uint32_t *)pc) == startins' \
  'la_load32_acq((const uint32_t *)&bc[(BCPos)foripos]) == fori'; do
  grep -F "$required" "$tuple_region" >/dev/null || {
    echo "ARM64 FORL tuple proof changed: $required" >&2
    exit 1
  }
done
test "$(grep -Fc 'trace_root_forl_tuple(' "$trace_source")" -eq 4

# Pin the narrow semantic grammar and its independent post-RA recheck.
awk '/^static int arm64_ir_forl_shape/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$asm_source" >"$forl_shape"
for required in \
  'nadd != 2u' \
  'step == 0' \
  'post.op1 != preadd || post.op2 != stepref' \
  'IRSLOAD_TYPECHECK|IRSLOAD_INHERIT' \
  'cmpop = step > 0 ? IR_LE : IR_GE;' \
  'IRSLOAD_READONLY|IRSLOAD_INHERIT' \
  '(int64_t)INT32_MAX-(int64_t)step' \
  '(int64_t)INT32_MIN-(int64_t)step' \
  'indexphi == 0'; do
  grep -F "$required" "$forl_shape" >/dev/null || {
    echo "ARM64 FORL semantic proof changed: $required" >&2
    exit 1
  }
done
awk '/^int lj_asm_arm64_postra_admit/ { copy=1 }
     copy { print }
     copy && /^static int arm64_ir_int_ref/ { exit }' \
  "$asm_source" >"$postra_region"
for required in \
  'rootop = bc_op(view->startins);' \
  'rootop != BC_LOOP && rootop != BC_FORL' \
  'rootop == BC_FORL && nadd != 2u' \
  'rootop == BC_LOOP && nadd != 0u' \
  'flags != 0 && flags != SNAP_NORESTORE' \
  'source.o != IR_SLOAD || source.op1 != slot' \
  'slot != forl_idxslot && slot != forl_idxslot+FORL_STOP'; do
  grep -F "$required" "$postra_region" >/dev/null || {
    echo "ARM64 FORL post-RA proof changed: $required" >&2
    exit 1
  }
done
grep -F 'TRACE_ARM64_INT_FORL_ADMITTED : TRACE_ARM64_INT_LOOP_ADMITTED;' \
  "$asm_source" >/dev/null

# Trace/mcode publication precedes the immutable recovery sidecar and one
# full-word bytecode CAS. FORI is intentionally left untouched in this stage.
awk '/^static int trace_stop/ { copy=1 }
     copy { print }
     copy && /^static / && !/^static int trace_stop/ { exit }' \
  "$trace_source" >"$trace_stop"
commit_line=$(grep -n 'lj_mcode_commit(J, J->cur.mcode);' "$trace_stop" | cut -d: -f1)
sync_line=$(grep -n 'lj_mcode_sync_core(J);' "$trace_stop" | cut -d: -f1)
save_line=$(grep -n 'trace_save(J, T);' "$trace_stop" | cut -d: -f1)
sidecar_line=$(grep -n 'proto_jit_startins_rel(pt, patchpc, J->cur.startins);' \
  "$trace_stop" | cut -d: -f1)
cas_line=$(grep -n 'BCIns observed = lj_bc_publish_cas_vm' "$trace_stop" | cut -d: -f1)
test "$commit_line" -lt "$sync_line" && test "$sync_line" -lt "$save_line" &&
test "$save_line" -lt "$sidecar_line" && test "$sidecar_line" -lt "$cas_line"
grep -F 'Leave FORI unpatched.' "$trace_stop" >/dev/null

# JFORL still performs its update/test and both integer/number taken paths tail
# to BC_JLOOP. Label 5 may recover the original branch only; it must not call
# the root-entry helper or allocate/authenticate a native-entry frame.
awk '/^  case BC_JFORI:/ { seen++; if (seen == 2) copy=1 }
     copy { print }
     copy && /^  case BC_ITERL:/ { exit }' "$vm_source" >"$forl_vm"
# The shared source has one integer JFORI/JFORL tail, then separate FP JFORI
# and JFORL tails. A generated JFORL receives exactly the first and last.
test "$(grep -Fc '=>BC_JLOOP' "$forl_vm")" -eq 3
grep -F 'str TMP0, FOR_IDX' "$forl_vm" >/dev/null
grep -F 'str TMP0, FOR_EXT' "$forl_vm" >/dev/null
awk '/\|5:  \/\/ JFOR\*\/JITERL already tested\/updated the edge/ { copy=1 }
     copy { print }
     copy && /^    \|\.endif/ { exit }' "$vm_source" >"$branch_only_vm"
for required in \
  'bl extern lj_trace_stale_startins' \
  'cmp TMP0w, #BC_JFORL' \
  'decode_RD RC, INS' \
  'sub PC, TMP0, #0x20000' \
  'arm64_vm_poll_acq TMP0w, TMP1w'; do
  grep -F "$required" "$branch_only_vm" >/dev/null || {
    echo "ARM64 JFORL branch-only recovery changed: $required" >&2
    exit 1
  }
done
if grep -E 'lj_trace_enter_root|sub sp, sp|br_trace_auth' \
     "$branch_only_vm" >/dev/null; then
  echo "ARM64 JFORL branch-only recovery gained native entry" >&2
  exit 1
fi

# The generic C root gate remains JLOOP/JFUNCF-only in Stage 1.
awk '/^static LJ_AINLINE int trace_root_entry_source_valid/ { copy=1 }
     copy { print }
     copy && /^static LJ_AINLINE GCtrace \*trace_root_entry_slot_acq/ { exit }' \
  "$trace_source" >"$root_entry"
grep -F 'BC_JLOOP' "$root_entry" >/dev/null
grep -F 'BC_JFUNCF' "$root_entry" >/dev/null
if grep -E 'BC_JFORL|TRACE_ARM64_INT_FORL_ADMITTED' "$root_entry" >/dev/null; then
  echo "ARM64 Stage 1 C root gate unexpectedly admits FORL" >&2
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
  'LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_FORL_NATIVE_ENTRY_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED 1'; do
  grep -E "^#define ${setting}$" "$macros" >/dev/null || {
    echo "ARM64 FORL gate mismatch: $setting" >&2
    exit 1
  }
done

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  -c "$fixture_source" -o "$fixture_obj"
"$cc" -arch arm64 -mmacosx-version-min="$minver" \
  "$fixture_obj" "$archive" -lm -pthread -o "$fixture"
ordinary_runs=${LJ_ARM64_FORL_RECORD_RUNS:-3}
run=1
while test "$run" -le "$ordinary_runs"; do
  "$fixture"
  run=$((run+1))
done
LUAJIT_MCODE_TEST=R "$fixture"
LJ_TEST_RUN_LOCK_HELD=1 "$root/tools/ci/arm64_jit_ir_admission_contract.sh"

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean \
    TARGET_FLAGS='-arch arm64e -mbranch-protection=bti' \
    XCFLAGS="$pauth_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" \
    TARGET_FLAGS='-arch arm64e -mbranch-protection=bti' \
    XCFLAGS="$pauth_xcflags"

# shellcheck disable=SC2086 # pauth_xcflags intentionally expands to arguments.
"$cc" -arch arm64e -mbranch-protection=bti \
  -mmacosx-version-min="$minver" $pauth_xcflags -I"$root/src" \
  -dM -E -x c -include lj_arch.h /dev/null >"$pauth_macros"
for setting in \
  'LJ_ABI_PAUTH 1' \
  'LJ_ABI_BRANCH_TRACK 1' \
  'LJ_ARM64_JIT_FORL_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_FORL_NATIVE_ENTRY_FAIL_CLOSED 1'; do
  grep -E "^#define ${setting}$" "$pauth_macros" >/dev/null || {
    echo "ARM64e FORL gate mismatch: $setting" >&2
    exit 1
  }
done

# shellcheck disable=SC2086 # pauth_xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64e \
  -mbranch-protection=bti -mmacosx-version-min="$minver" \
  $pauth_xcflags -I"$root/src" -c "$fixture_source" -o "$pauth_obj"
"$cc" -arch arm64e -mbranch-protection=bti \
  -mmacosx-version-min="$minver" "$pauth_obj" "$archive" -lm -pthread \
  -o "$pauth_fixture"
otool -hv "$pauth_fixture" | grep -E 'ARM64[[:space:]]+E' >/dev/null
pauth_runs=${LJ_ARM64_FORL_RECORD_PAUTH_RUNS:-2}
run=1
while test "$run" -le "$pauth_runs"; do
  "$pauth_fixture"
  run=$((run+1))
done
LUAJIT_MCODE_TEST=R "$pauth_fixture"

# Leave the shared checkout in the ordinary experimental configuration.
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' XCFLAGS="$xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$xcflags"
restore_needed=0

echo "arm64_jit_forl_record_contract OK: bounded FORL roots published on ARM64/ARM64e; JFORL stayed branch-only"
