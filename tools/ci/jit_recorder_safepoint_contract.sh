#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-recorder-safepoint.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

trace_hot=$tmpdir/trace-hot.c
trace_state=$tmpdir/trace-state.c
trace_side=$tmpdir/trace-side.c
trace_stitch=$tmpdir/trace-stitch.c
trace_exit=$tmpdir/trace-exit.c
trace_ins=$tmpdir/trace-ins.c
hotcall=$tmpdir/hotcall.c
signal_late=$tmpdir/signal-late.c
handshake=$tmpdir/handshake.c

sed -n '/void LJ_FASTCALL lj_trace_hot/,/static void trace_hotside/p' \
  "$root/src/lj_trace.c" >"$trace_hot"
sed -n '/static TValue \*trace_state/,/void lj_trace_abort_owner/p' \
  "$root/src/lj_trace.c" >"$trace_state"
sed -n '/static void trace_hotside/,/uint32_t LJ_FASTCALL lj_trace_stitch_probe/p' \
  "$root/src/lj_trace.c" >"$trace_side"
sed -n '/void LJ_FASTCALL lj_trace_stitch(/,/Tiny struct to pass data/p' \
  "$root/src/lj_trace.c" >"$trace_stitch"
sed -n '/\/\* A trace exited/,/\/\* --/p' \
  "$root/src/lj_trace.c" >"$trace_exit"
sed -n '/void lj_trace_ins/,/static int trace_hot_root_start_valid/p' \
  "$root/src/lj_trace.c" >"$trace_ins"
sed -n '/static void call_hot_poll/,/^}/p' \
  "$root/src/lj_dispatch.c" >"$hotcall"
sed -n '/static uint32_t safepoint_signal_late/,/static int safepoint_native_ack_allowed/p' \
  "$root/src/lj_safepoint.c" >"$signal_late"
sed -n '/uint32_t lj_safepoint_handshake/,/^}/p' \
  "$root/src/lj_safepoint.c" >"$handshake"

line_of() {
  file=$1
  pattern=$2
  occurrence=${3:-1}
  grep -n -F "$pattern" "$file" | sed -n "${occurrence}s/:.*//p"
}

if grep -F 'LJ_ARM64_JIT_RECORDER_ADMISSION_FAIL_CLOSED' \
     "$root/src/lj_trace.c" "$root/tests/t-jit-recorder-safepoint.c" \
     >/dev/null; then
  echo "recorder safepoint coverage still uses the aggregate recorder gate" >&2
  exit 1
fi
for gate_region in \
  "LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED:$trace_hot" \
  "LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED:$trace_ins" \
  "LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED:$trace_side" \
  "LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED:$trace_stitch"; do
  gate=${gate_region%%:*}
  region=${gate_region#*:}
  grep -F "#if $gate" "$region" >/dev/null || {
    echo "recorder safepoint contract lost granular gate $gate" >&2
    exit 1
  }
done

entry_ack=$(line_of "$trace_hot" 'lj_safepoint_ack_check(L)' 1)
# Match both the current global setter and the generation-aware owner-local
# setter used by the hotcount tranche; only the ordering is this contract's.
hotcount_reset=$(line_of "$trace_hot" 'hotcount_set' 1)
token_try=$(line_of "$trace_hot" 'lj_jit_token_try_l(L, J)' 1)
late_release=$(line_of "$trace_hot" 'lj_jit_token_release_l(L, J)' 1)
late_ack=$(line_of "$trace_hot" 'lj_safepoint_ack_check(L)' 2)
start_publish=$(line_of "$trace_hot" 'lj_trace_state_store_active(J, LJ_TRACE_START)' 1)

for value in "$entry_ack" "$hotcount_reset" "$token_try" "$late_release" \
             "$late_ack" "$start_publish"; do
  test -n "$value" || {
    echo "recorder safepoint contract lost an admission edge" >&2
    exit 1
  }
done

test "$entry_ack" -lt "$hotcount_reset"
test "$hotcount_reset" -lt "$token_try"
test "$token_try" -lt "$late_release"
test "$late_release" -lt "$late_ack"
test "$late_ack" -lt "$start_publish"

state_ack=$(line_of "$trace_state" 'lj_safepoint_ack_check(L)' 1)
state_machine=$(line_of "$trace_state" 'do {' 1)
test -n "$state_ack" && test -n "$state_machine"
test "$state_ack" -lt "$state_machine"

if grep -F 'trace_poll_pending' "$root/src/lj_trace.c" >/dev/null; then
  echo "side admission regressed to the poll-only predicate" >&2
  exit 1
fi

side_outer_inject=$(line_of "$trace_exit" 'trace_test_side_admission_inject(' 1)
side_outer_pending=$(line_of "$trace_exit" 'lj_safepoint_owner_poll_pending(L)' 1)
side_outer_rearm=$(line_of "$trace_exit" 'lj_safepoint_owner_rearm_counted_poll(L)' 1)
side_call=$(line_of "$trace_exit" 'trace_hotside(J, pc, L, parent, exitno)' 1)
test -n "$side_outer_inject" && test -n "$side_outer_pending" && \
  test -n "$side_outer_rearm" && test -n "$side_call"
test "$side_outer_inject" -lt "$side_outer_pending"
test "$side_outer_pending" -lt "$side_outer_rearm"
test "$side_outer_rearm" -lt "$side_call"

arm_side_gate=$(line_of "$trace_side" '#if LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED' 1)
arm_side_return=$(line_of "$trace_side" 'return;' 1)
side_smr_try=$(line_of "$trace_side" '!lj_gc2_smr_read_try(g)' 1)
inner_pending=$(line_of "$trace_side" 'lj_safepoint_owner_poll_pending(L)' 1)
first_snap_cas=$(line_of "$trace_side" 'snap_count_cas_acqrel(' 1)
side_token=$(line_of "$trace_side" 'lj_jit_token_try_l(L, J)' 1)
late_pending=$(line_of "$trace_side" 'lj_safepoint_owner_poll_pending(L)' 2)
late_release=$(line_of "$trace_side" 'lj_jit_token_release_l(L, J)' 1)
late_smr_leave=$(line_of "$trace_side" 'lj_gc2_smr_read_leave(g)' 2)
late_rearm=$(line_of "$trace_side" 'lj_safepoint_owner_rearm_counted_poll(L)' 2)
side_stream_recheck=$(line_of "$trace_side" 'lj_jit_trace_stream_idle(g)' 2)
second_snap_cas=$(line_of "$trace_side" 'snap_count_cas_acqrel(' 2)
for value in "$arm_side_gate" "$arm_side_return" "$side_smr_try" "$inner_pending" \
             "$first_snap_cas" "$side_token" "$late_pending" \
             "$late_release" "$late_smr_leave" "$late_rearm" \
             "$side_stream_recheck" "$second_snap_cas"; do
  test -n "$value" || {
    echo "recorder safepoint contract lost a side-admission edge" >&2
    exit 1
  }
done
test "$arm_side_gate" -lt "$arm_side_return"
test "$arm_side_return" -lt "$side_smr_try"
test "$inner_pending" -lt "$first_snap_cas"
test "$side_token" -lt "$late_pending"
test "$late_pending" -lt "$late_release"
test "$late_release" -lt "$late_smr_leave"
test "$late_smr_leave" -lt "$late_rearm"
test "$late_rearm" -lt "$side_stream_recheck"
test "$late_rearm" -lt "$second_snap_cas"
if grep -F 'lj_safepoint_ack_check(L)' "$trace_side" >/dev/null; then
  echo "trace_hotside acknowledges while jit_base is still published" >&2
  exit 1
fi

stitch_gate=$(line_of "$trace_stitch" '#if LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED' 1)
stitch_abort=$(line_of "$trace_stitch" 'lj_trace_abort_owner(L)' 1)
stitch_return=$(line_of "$trace_stitch" 'return;' 1)
test -n "$stitch_gate" && test -n "$stitch_abort" && test -n "$stitch_return"
test "$stitch_gate" -lt "$stitch_abort" && test "$stitch_abort" -lt "$stitch_return"

error_save=$(line_of "$trace_ins" 'lj_oserr_save(&oserr)' 1)
error_cleanup=$(line_of "$trace_ins" 'lj_trace_abort_owner(L)' 1)
error_restore=$(line_of "$trace_ins" 'lj_oserr_restore(&oserr)' 1)
error_throw=$(line_of "$trace_ins" 'lj_err_throw(L, errcode)' 1)
for value in "$error_save" "$error_cleanup" "$error_restore" "$error_throw"; do
  test -n "$value" || {
    echo "recorder external-error cleanup lost OS-error preservation" >&2
    exit 1
  }
done
test "$error_save" -lt "$error_cleanup"
test "$error_cleanup" -lt "$error_restore"
test "$error_restore" -lt "$error_throw"

grep -F 'lj_safepoint_owner_poll_pending(L)' "$hotcall" >/dev/null
fill=$(line_of "$hotcall" 'call_fill_missing(L, missing)' 1)
call_ack=$(line_of "$hotcall" 'lj_safepoint_ack_check(L)' 1)
test -n "$fill" && test -n "$call_ack" && test "$fill" -lt "$call_ack"

for field in poll reqmask profile_request; do
  grep -F "lj_tg_${field}_acq(tg)" "$root/src/lj_safepoint.h" >/dev/null
done
sed -n '/lj_safepoint_owner_rearm_counted_poll/,/^}/p' \
  "$root/src/lj_safepoint.h" >"$tmpdir/rearm.c"
grep -F 'lj_tg_reqmask_acq(tg)' "$tmpdir/rearm.c" >/dev/null
grep -F 'lj_tg_poll_rel(tg, 1)' "$tmpdir/rearm.c" >/dev/null

signal_reqmask=$(line_of "$signal_late" 'lj_tg_reqmask_rel(tg, actions)' 1)
signal_pause=$(line_of "$signal_late" 'safepoint_test_signal_pause_after_reqmask(tg)' 1)
signal_poll=$(line_of "$signal_late" 'lj_tg_poll_rel(tg, 1)' 1)
test -n "$signal_reqmask" && test -n "$signal_pause" && test -n "$signal_poll"
test "$signal_reqmask" -lt "$signal_pause"
test "$signal_pause" -lt "$signal_poll"

final_poll_clear=$(line_of "$handshake" 'safepoint_clear_consumed_polls(g, epoch)' 1)
final_clean_note=$(line_of "$handshake" 'safepoint_test_signal_note_before_leader_leave(g, epoch)' 1)
leader_leave=$(line_of "$handshake" 'safepoint_leader_leave(g, leader)' 1)
test -n "$final_poll_clear" && test -n "$final_clean_note" && \
  test -n "$leader_leave"
test "$final_poll_clear" -lt "$final_clean_note"
test "$final_clean_note" -lt "$leader_leave"
grep -F 'LJ_TRACE_TEST_REQUEST_OBSERVE' "$root/src/lj_trace.c" >/dev/null

for vm in vm_x64.dasc vm_arm64.dasc; do
  block=$tmpdir/$vm.hotloop
  sed -n '/|->vm_hotloop:/,/|->vm_callhook:/p' "$root/src/$vm" >"$block"
  if grep -F 'vm_safepoint' "$block" >/dev/null; then
    echo "$vm routes vm_hotloop through generic vm_safepoint" >&2
    exit 1
  fi
  grep -F 'extern lj_trace_hot' "$block" >/dev/null
done
grep -F 'jmp <3' "$tmpdir/vm_x64.dasc.hotloop" >/dev/null
grep -F 'b <3' "$tmpdir/vm_arm64.dasc.hotloop" >/dev/null

for vm in vm_x64.dasc vm_arm64.dasc; do
  block=$tmpdir/$vm.exit-interp
  sed -n '/|->vm_exit_interp:/,/|->vm_exit_rethrow:/p' \
    "$root/src/$vm" >"$block"
  clear=$(line_of "$block" 'jit_base' 1)
  ack=$(line_of "$block" 'lj_safepoint_ack_check' 1)
  test -n "$clear" && test -n "$ack" && test "$clear" -lt "$ack"
done

echo "jit_recorder_safepoint_contract OK: granular root/side/stitch gates and cleanup ordered"
