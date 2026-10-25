#!/bin/sh
set -eu
root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip arm64_jit_forl_record_contract; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi
lj_ci_profile_init "$root" arm64_jit_forl_record_contract
lj_ci_tmpdir lj-arm64-forl-record
tmpdir=$LJ_CI_TMPDIR
root=$LJ_CI_ROOT
archive=$LJ_CI_ARCHIVE
fixture_source=$root/tests/t-arm64-jit-forl-record.c
trace_source=$root/src/lj_trace.c
asm_source=$root/src/lj_asm.c
vm_source=$root/src/vm_arm64.dasc
minver=$LJ_CI_MINVER
xcflags=${LJ_ARM64_FORL_XCFLAGS:--DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS -DLUAJIT_MCODE_TEST}
pauth_xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR"
restore_target=${LJ_ARM64_FORL_RESTORE_TARGET_FLAGS:-${LJ_TEST_RESTORE_TARGET_FLAGS:--arch arm64}}
restore_xcflags=${LJ_ARM64_FORL_RESTORE_XCFLAGS:-${LJ_TEST_RESTORE_XCFLAGS:-$xcflags}}
lj_ci_set_restore "$restore_target" "$restore_xcflags"
fixture=$tmpdir/t-arm64-jit-forl-record
pauth_fixture=$tmpdir/t-arm64-jit-forl-record-arm64e
profile_check=$tmpdir/profile-check.c
forl_vm=$tmpdir/forl-vm.txt
branch_only_vm=$tmpdir/branch-only-vm.txt
root_entry=$tmpdir/root-entry.txt
tuple_region=$tmpdir/tuple-region.txt
trace_stop=$tmpdir/trace-stop.txt
cat >"$profile_check" <<'EOF'
#include "lj_arch.h"
#if !LJ_TARGET_ARM64
#error "not ARM64"
#endif
#if LJ_ABI_PAUTH != EXPECT_PAUTH
#error "unexpected pointer-authentication ABI"
#endif
#if LJ_ABI_BRANCH_TRACK != EXPECT_BRANCH
#error "unexpected branch-tracking ABI"
#endif
#if LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED != 0
#error "root recorder gate is closed"
#endif
#if LJ_ARM64_JIT_FORL_RECORDER_FAIL_CLOSED != 0
#error "FORL recorder gate is closed"
#endif
#if LJ_ARM64_JIT_FUNCF_RECORDER_FAIL_CLOSED != 0
#error "FUNCF recorder gate is closed"
#endif
#if LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED != 1
#error "side recorder gate unexpectedly opened"
#endif
#if LJ_ARM64_JIT_FIRST_SIDE_RECORDER_FAIL_CLOSED != 0
#error "first-side recorder gate is closed"
#endif
#if LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED != 1
#error "stitch recorder gate unexpectedly opened"
#endif
#if LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED != 0
#error "loop native-entry gate is closed"
#endif
#if LJ_ARM64_JIT_FORL_NATIVE_ENTRY_FAIL_CLOSED != 0
#error "FORL native-entry gate is closed"
#endif
#if LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED != 0
#error "JFUNCF native-entry gate is closed"
#endif
#if LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED != 1
#error "stitch native-entry gate unexpectedly opened"
#endif
int profile_check;
EOF

# Freeze the recorder/publication proof independently from the native-entry
# contract. Integer roots enter their certified target; FP JFORL remains on
# branch-only recovery.
for required in \
  'LJ_ARM64_JIT_FORL_RECORDER_FAIL_CLOSED' \
  'LJ_ARM64_JIT_FORL_NATIVE_ENTRY_FAIL_CLOSED' \
  'TRACE_ARM64_INT_FORL_ADMITTED' \
  'TRACE_ARM64_INT_LOOP_ADMITTED' \
  'proto_jit_startins_acq(pt, forlpc) == forl' \
  'memcpy(&pcbase, &map[positive_mapofs[sn]+positive_nent[sn]],' \
  'SNAP_NORESTORE, R_IDX' \
  'SNAP_NORESTORE, R_STOP' \
  'SNAP_NORESTORE, D_STEP' \
  'expect_ir(ir, D_USE, IR_USE, IRT_INT, D_OVERFLOW, 0);' \
  'assert(ir[D_USE].r == RID_INIT && !ra_hasspill(ir[D_USE].s));' \
  'static void run_dynamic(int negative)' \
  'run_dynamic(0);' \
  'run_dynamic(1);' \
  'negative ? (lua_Integer)INT32_MIN : (lua_Integer)INT32_MAX' \
  'static void expect_native_entry(void)' \
  'static void expect_branch_only(void)' \
  'lj_trace_test_exit_calls() ==' \
  'lj_trace_test_root_entry_startins_calls() == 0' \
  'callnum1(L, "__arm64_forl_positive", 3.5) == 6.0' \
  'lj_trace_test_root_entry_startins_calls() == 2' \
  'run_lua(L, "jit.flush()\n");' \
  'assert((BCIns)la_load32_acq((const uint32_t *)forlpc) == forl);' \
  'assert(trace_startpc_acq(T) == forlpc);' \
  'function __arm64_forl_negative_stop(n, stop)' \
  'bound = find_kint(T, INT32_MIN+3);' \
  'function __arm64_forl_constant_stop(a, step)' \
  'call2(L, "__arm64_forl_constant_stop", 1, 2) == 400' \
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
test "$(grep -Fc 'trace_root_forl_tuple(' "$trace_source")" -eq 3

# The executable IR admission contract below checks the generic FORL graph
# and its independent post-RA proof, including dynamic step direction,
# overflow, snapshot ownership and allocated-location mutations. Do not pin
# instruction counts or adjacency: unrelated scalar operations may compose
# with the same control graph.
for required in \
  'trace_arm64_admission_rel(T, TRACE_ARM64_INT_FORL_ADMITTED);' \
  'trace_arm64_admission_rel(T, TRACE_ARM64_TRUE_FUNCF_ADMITTED);' \
  'trace_arm64_admission_rel(T, TRACE_ARM64_INT_LOOP_ADMITTED);'; do
  grep -F "$required" "$asm_source" >/dev/null
done

# Trace/mcode publication precedes the immutable recovery sidecar and one
# full-word bytecode CAS. FORI is intentionally left untouched in this stage.
awk '/^static int trace_stop\(jit_State \*J\)/ { copy=1 }
     copy { print }
     copy && /^static / && !/^static int trace_stop\(jit_State \*J\)/ { exit }' \
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

# JFORL still performs its update/test before native admission. The integer
# taken edge has one dedicated helper and the two FP signs still tail through
# label 5, whose recovery must remain branch-only.
awk '/^  case BC_JFORI:/ { seen++; if (seen == 2) copy=1 }
     copy { print }
     copy && /^  case BC_ITERL:/ { exit }' "$vm_source" >"$forl_vm"
# JFORI, both FP signs and the integer-helper rejection each have one tail.
test "$(grep -Fc '=>BC_JLOOP' "$forl_vm")" -eq 4
test "$(grep -Fc 'bl extern lj_trace_enter_root' "$forl_vm")" -eq 1
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

# The generic C root gate admits JFORL only under its independent native gate.
awk '/^static LJ_AINLINE int trace_root_entry_source_valid/ { copy=1 }
     copy { print }
     copy && /^static LJ_AINLINE GCtrace \*trace_root_entry_slot_acq/ { exit }' \
  "$trace_source" >"$root_entry"
grep -F 'BC_JLOOP' "$root_entry" >/dev/null
grep -F 'BC_JFUNCF' "$root_entry" >/dev/null
grep -F 'BC_JFORL' "$root_entry" >/dev/null
grep -F '!LJ_ARM64_JIT_FORL_NATIVE_ENTRY_FAIL_CLOSED' \
  "$root_entry" >/dev/null


build_and_run() {
  arch=$1
  target_flags=$2
  flags=$3
  tag=$4
  pauth=$5
  branch=$6
  lj_ci_build_profile "$arch" "$target_flags" "$flags" "$tag"
  lj_ci_compile_object "$tmpdir/profile-$tag.o" "$profile_check" \
    "-DEXPECT_PAUTH=$pauth" "-DEXPECT_BRANCH=$branch"
  fixture=$tmpdir/t-arm64-jit-forl-record-$tag
  lj_ci_compile_fixture "$fixture" "$fixture_source"
  otool -hv "$fixture" | grep -E 'ARM64[[:space:]]+(ALL|E)' >/dev/null
  if test "$tag" = arm64; then
    ordinary_runs=${LJ_ARM64_FORL_RECORD_RUNS:-3}
  else
    ordinary_runs=${LJ_ARM64_FORL_RECORD_PAUTH_RUNS:-2}
  fi
  run=1
  while test "$run" -le "$ordinary_runs"; do
    lj_ci_run_fixture 120 "$fixture"
    run=$((run+1))
  done
  LUAJIT_MCODE_TEST=R lj_ci_run_fixture 120 "$fixture"
  if test "$tag" = arm64; then
    LJ_TEST_RUN_LOCK_HELD=1 LJ_TEST_XCFLAGS="$flags" \
      sh "$root/tools/ci/arm64_jit_ir_admission_contract.sh"
  fi
}

vm_object=$root/src/lj_vm.o
build_and_run arm64 '-arch arm64' "$xcflags" arm64 0 0
build_and_run arm64e '-arch arm64e -mbranch-protection=bti' \
  "$pauth_xcflags" arm64e 1 1
lj_ci_restore "$restore_target" "$restore_xcflags"
echo "arm64_jit_forl_record_contract OK: bounded FORL roots published and entered on ARM64/ARM64e; FP stayed branch-only"
