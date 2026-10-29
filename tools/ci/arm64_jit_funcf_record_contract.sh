#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip arm64_jit_funcf_record_contract; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi
lj_ci_profile_init "$root" arm64_jit_funcf_record_contract
lj_ci_tmpdir lj-arm64-funcf-record
tmpdir=$LJ_CI_TMPDIR
root=$LJ_CI_ROOT
archive=$LJ_CI_ARCHIVE
vm_object=$root/src/lj_vm.o
fixture_source=$root/tests/t-arm64-jit-funcf-record.c
funcf_call_source=$root/tests/t-arm64-jit-funcf-calls.c
trace_source=$root/src/lj_trace.c
asm_source=$root/src/lj_asm.c
admit_source=$root/src/lj_asm_arm64_admit.h
arch_source=$root/src/lj_arch.h
vm_source=$root/src/vm_arm64.dasc
minver=$LJ_CI_MINVER
xcflags=${LJ_ARM64_FUNCF_XCFLAGS:--DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS -DLUAJIT_MCODE_TEST}
pauth_xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR"
restore_target=${LJ_ARM64_FUNCF_RESTORE_TARGET_FLAGS:-${LJ_TEST_RESTORE_TARGET_FLAGS:--arch arm64}}
restore_xcflags=${LJ_ARM64_FUNCF_RESTORE_XCFLAGS:-${LJ_TEST_RESTORE_XCFLAGS:-$xcflags}}
lj_ci_set_restore "$restore_target" "$restore_xcflags"

fixture=$tmpdir/t-arm64-jit-funcf-record
pauth_fixture=$tmpdir/t-arm64-jit-funcf-record-arm64e
vm_disasm=$tmpdir/vm-arm64.disasm
jfuncf_source=$tmpdir/jfuncf-source.txt
jfuncf_disasm=$tmpdir/vm-arm64-jfuncf.disasm
vm_reloc=$tmpdir/vm-arm64.reloc
pauth_vm_disasm=$tmpdir/vm-arm64e.disasm
pauth_jfuncf_disasm=$tmpdir/vm-arm64e-jfuncf.disasm
profile_check=$tmpdir/profile-check.c

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
#if LJ_ARM64_JIT_FUNCF_RECORDER_FAIL_CLOSED != 0
#error "FUNCF recorder gate is closed"
#endif
#if LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED != 0
#error "JFUNCF native entry gate is closed"
#endif
int profile_check;
EOF

# The C fixture itself exercises actual native publication for each supported
# scalar operation and value class, plus fail-closed mutations and entry races.
# Keep these checks about the evidence being run, not one particular IR layout.
for required in \
  'run_scalar_positives();' \
  'run_rejections();' \
  'run_integer_addov_native();' \
  '__arm64_funcf_false' \
  '__arm64_funcf_nil' \
  '__arm64_funcf_number' \
  '__arm64_funcf_argument' \
  '__arm64_funcf_add' \
  '__arm64_funcf_sub' \
  '__arm64_funcf_mul' \
  '__arm64_funcf_div' \
  '__arm64_funcf_multi' \
  '__arm64_funcf_immediate' \
  '__arm64_funcf_integer_knum' \
  '__arm64_funcf_addvn' \
  '__arm64_funcf_subvn' \
  '__arm64_funcf_subnv' \
  '__arm64_funcf_divvn' \
  '__arm64_funcf_divnv' \
  '__arm64_funcf_addnv' \
  '__arm64_funcf_live_interference' \
  'test_funcf_generation_race' \
  'test_disabled_unsupported_funcf_hotpatch();' \
  'ir[load].op1++' \
  'ir[result].prev = ir[arg].prev' \
  'ir[right].prev = ir[left].prev' \
  'ir[mul].r = ir[live_ref].r' \
  'assert(!lj_asm_arm64_postra_funcf_entry_admit(&view, live, NULL))' \
  'TRACE_ARM64_TRUE_FUNCF_ADMITTED' \
  'LJ_TRACE_ROOT_ENTRY_PAUSE_POSTMETADATA' \
  'LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION' \
  'test_funcf_stale_top_geometry' \
  'bad_frame_top' \
  'test_funcf_native_xpoll' \
  'test_funcf_native_stopreq'; do
  grep -F "$required" "$fixture_source" >/dev/null || {
    echo "ARM64 FUNCF executable proof lost required case: $required" >&2
    exit 1
  }
done
grep -F 'test_pair_output_liveness' "$funcf_call_source" >/dev/null || {
  echo "ARM64 FUNCF pair-output liveness regression case missing" >&2
  exit 1
}

# Both independent validators derive the returned scalar from bytecode
# definitions and validate concrete snapshots/allocator state. Runtime
# positives and post-RA corruption/recovery execute these paths; source checks
# only ensure neither independent walker disappeared.
for required in \
  'arm64_ir_funcf_advance' \
  'arm64_ir_funcf_result_matches' \
  'arm64_ir_funcf_scalar_snapshots' \
  'arm64_postra_funcf_admit' \
  'arm64_postra_funcf_locations' \
  'arm64_scalar_postra_register' \
  'arm64_ir_funcf_bytecode'; do
  grep -F "$required" "$admit_source" >/dev/null || {
    echo "ARM64 FUNCF operation validator lost required proof: $required" >&2
    exit 1
  }
done

# Reject regressions to the former exact-three-word true-return template.
if grep -E 'pt->sizebc != 3|T->nins != REF_BASE\+3u|nsnap != 2 \|\| nsnapmap != 5|SNAP\(result_slot, 0, REF_TRUE\)' \
     "$admit_source" "$trace_source" >/dev/null; then
  echo "ARM64 FUNCF operation admission reverted to the exact-true template" >&2
  exit 1
fi

for required in \
  'LJ_ARM64_JIT_FUNCF_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED 0'; do
  setting=${required%% *}
  value=${required##* }
  grep -E "^#define[[:space:]]+$setting[[:space:]]+$value$" \
    "$arch_source" >/dev/null || {
    echo "ARM64 FUNCF gate mismatch: $required" >&2
    exit 1
  }
done

# Native JFUNCF entry remains a separate VM boundary proof. The executable
# fixture checks both entry lifecycle and post-ADMISSION STOPREQ behavior.
grep -F 'trace_arm64_entrybc_matches' "$trace_source" >/dev/null
grep -F 'lj_asm_arm64_postra_funcf_entry_admit(' "$trace_source" >/dev/null
grep -F 'sourceins == v->startins' "$trace_source" >/dev/null
grep -F '&& prepublication)' "$trace_source" >/dev/null
grep -F 'sourceop == (uint32_t)BC_JFUNCF' "$trace_source" >/dev/null
grep -F 'frame_topp != expected_top' "$trace_source" >/dev/null
grep -F 'LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED' "$vm_source" >/dev/null
awk '/case BC_JFUNCF:/ { seen++; if (seen == 2) copy=1 }
     copy { print }
     copy && /case BC_FUNCV:/ { exit }' \
  "$vm_source" >"$jfuncf_source"
for required in \
  'bl extern lj_trace_enter_root' \
  'mov CARG7, RA' \
  'sub sp, sp, #16' \
  'br_trace_auth CARG2, CRET1'; do
  grep -F "$required" "$jfuncf_source" >/dev/null || {
    echo "ARM64 JFUNCF native boundary lost required instruction: $required" >&2
    exit 1
  }
done


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
  fixture=$tmpdir/t-arm64-jit-funcf-record-$tag
  lj_ci_compile_fixture "$fixture" "$fixture_source"
  funcf_call_fixture=$tmpdir/t-arm64-jit-funcf-calls-$tag
  lj_ci_compile_fixture "$funcf_call_fixture" "$funcf_call_source"
  if test "$tag" = arm64; then
    nm "$vm_object" | grep ' U _lj_trace_enter_root$' >/dev/null
    otool -rv "$vm_object" >"$vm_reloc"
    grep 'BR26.*_lj_trace_enter_root$' "$vm_reloc" >/dev/null
    otool -tvV "$vm_object" >"$vm_disasm"
    awk '/^_lj_BC_JFUNCF:/ { copy=1 }
         copy { print }
         copy && /^_lj_BC_(FUNCV|IFUNCV):/ { exit }' \
      "$vm_disasm" >"$jfuncf_disasm"
    test -s "$jfuncf_disasm"
    grep -E 'sub[[:space:]]+sp, sp, #0x10' "$jfuncf_disasm" >/dev/null
    grep -E 'br[[:space:]]+x1$' "$jfuncf_disasm" >/dev/null
    if grep -E 'braa[[:space:]]+x1, x0$' "$jfuncf_disasm" >/dev/null; then
      echo "ordinary JFUNCF unexpectedly uses an authenticated transfer" >&2
      return 1
    fi
  else
    otool -tvV "$vm_object" >"$pauth_vm_disasm"
    awk '/^_lj_BC_JFUNCF:/ { copy=1 }
         copy { print }
         copy && /^_lj_BC_(FUNCV|IFUNCV):/ { exit }' \
      "$pauth_vm_disasm" >"$pauth_jfuncf_disasm"
    test -s "$pauth_jfuncf_disasm"
    grep -E 'bti[[:space:]]+j' "$pauth_jfuncf_disasm" >/dev/null
    grep -E 'sub[[:space:]]+sp, sp, #0x10' "$pauth_jfuncf_disasm" >/dev/null
    grep -E 'braa[[:space:]]+x1, x0$' "$pauth_jfuncf_disasm" >/dev/null
    if grep -E 'br[[:space:]]+x1$|braaz[[:space:]]+x1$' \
         "$pauth_jfuncf_disasm" >/dev/null; then
      echo "arm64e JFUNCF lost its trace-discriminated transfer" >&2
      return 1
    fi
  fi
  otool -hv "$fixture" | grep -E 'ARM64[[:space:]]+(ALL|E)' >/dev/null
  otool -tvV "$fixture" | grep '^_main:' >/dev/null
  if test "$tag" = arm64; then
    ordinary_runs=${LJ_ARM64_FUNCF_RECORD_RUNS:-3}
  else
    ordinary_runs=${LJ_ARM64_FUNCF_RECORD_PAUTH_RUNS:-2}
  fi
  run=1
  while test "$run" -le "$ordinary_runs"; do
    lj_ci_run_fixture 120 "$fixture" default
    run=$((run+1))
  done
  LUAJIT_MCODE_TEST=R lj_ci_run_fixture 120 "$fixture" randomized
  LUA_PATH="$root/src/?.lua;;" "$root/src/luajit" \
    "$root/bench/jit-topology-probe.lua" >"$tmpdir/topology-$tag.tsv"
  lj_ci_run_fixture 120 "$funcf_call_fixture" default
}

build_and_run arm64 '-arch arm64' "$xcflags" arm64 0 0
build_and_run arm64e '-arch arm64e -mbranch-protection=bti' \
  "$pauth_xcflags" arm64e 1 1
lj_ci_restore "$restore_target" "$restore_xcflags"
echo "arm64_jit_funcf_record_contract OK: scalar FUNCF proofs passed natively on ARM64/ARM64e"
