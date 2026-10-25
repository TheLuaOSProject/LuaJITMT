#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_ir_admission_contract SKIP: requires native macOS arm64"
  exit 0
fi

if test -z "${SDKROOT:-}"; then
  SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
  export SDKROOT
fi

cc=${CC:-$(xcrun --sdk macosx --find clang)}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
archive=$root/src/libluajit.a
asm_object=$root/src/lj_asm.o
asm_source=$root/src/lj_asm.c
admit_source=$root/src/lj_asm_arm64_admit.h
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-ir-admission.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

fixture=$tmpdir/t-arm64-jit-ir-admission
trace_asm=$tmpdir/trace-asm.txt
call_region=$tmpdir/call-region.txt
arm64_cnew_region=$tmpdir/arm64-cnew-region.txt
positive_region=$tmpdir/positive-region.txt
dynamic_forl_region=$tmpdir/dynamic-forl-region.txt
dynamic_forl_tests=$tmpdir/dynamic-forl-tests.txt
pauth_macros=$tmpdir/macros-arm64e.txt
audit_object=$tmpdir/lj_asm-arm64e.o
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT'
pauth_xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR"

test -f "$archive" && test -f "$asm_object" || {
  echo "ARM64 IR admission contract requires an existing experimental build" >&2
  exit 1
}
test "$asm_object" -nt "$asm_source" &&
test "$asm_object" -nt "$admit_source" || {
  echo "ARM64 IR admission object is stale relative to its sources" >&2
  exit 1
}
ar -p "$archive" lj_asm.o | cmp - "$asm_object" || {
  echo "ARM64 IR admission archive does not contain current lj_asm.o" >&2
  exit 1
}

nm "$archive" | grep ' T _lj_asm_arm64_ir_admit$' >/dev/null || {
  echo "experimental archive lacks the ARM64 IR admission gate" >&2
  exit 1
}
nm "$archive" | grep ' T _lj_asm_arm64_postra_admit$' >/dev/null || {
  echo "experimental archive lacks the ARM64 post-RA spill gate" >&2
  exit 1
}

# The compiled fixture below exercises independently authored semantic and
# allocated-state mutations. Runtime rooted-operation contracts cover changes
# in key/result class, helper count, table count, and scratch-slot composition;
# source spelling and example IR ordinals are not acceptance criteria.

# Keep the production check ahead of IR scratch growth, compact-trace
# allocation and mcode reservation/publication.
awk '/^void lj_asm_trace\(/ { copying = 1 }
     copying { print }
     copying && /^#if LJ_TARGET_ARM64 && defined\(LJ_ARM64_EMIT_TEST_HELPERS\)/ {
       exit
     }' "$asm_source" >"$trace_asm"
admit_line=$(grep -n 'lj_asm_arm64_ir_admit(J, T, &reject)' "$trace_asm" | cut -d: -f1)
nextins_line=$(grep -n 'as->orignins = lj_ir_nextins(J)' "$trace_asm" | cut -d: -f1)
reserve_line=$(grep -n 'lj_mcode_reserve(J, &as->mcbot)' "$trace_asm" | cut -d: -f1)
test -n "$admit_line" && test "$admit_line" -lt "$nextins_line" &&
test "$admit_line" -lt "$reserve_line"
grep -F 'lj_trace_err_info(J, LJ_TRERR_NYIIR);' "$trace_asm" >/dev/null
for required in \
  'finalview.nins = arm64_semantic_nins;' \
  'postraview.ir = finalir;' \
  'postraview.proto_bc = proto_bc(J->pt);' \
  'postraview.nins = finalnins;' \
  'postraview.nk = T->nk;' \
  'postraview.spadjust = T->spadjust;' \
  'postraview.proto_sizebc = J->pt->sizebc;' \
  'postraview.root_topslot = T->topslot;' \
  'postraview.startins = T->startins;' \
  'postraview.base_delta = (uint8_t)(J->baseslot-2u);' \
  '!lj_asm_arm64_postra_admit(' \
  'validated_semantic_nins != arm64_semantic_nins' \
  'if (bc_op(T->startins) == BC_FORL)' \
  'trace_arm64_admission_rel(T, TRACE_ARM64_INT_FORL_ADMITTED);' \
  'else if (bc_op(T->startins) == BC_FUNCF)' \
  'trace_arm64_admission_rel(T, TRACE_ARM64_TRUE_FUNCF_ADMITTED);' \
  'trace_arm64_admission_rel(T, TRACE_ARM64_INT_LOOP_ADMITTED);'; do
  grep -F "$required" "$trace_asm" >/dev/null || {
    echo "ARM64 post-RA admission check changed: $required" >&2
    exit 1
  }
done

postra_line=$(grep -n '!lj_asm_arm64_postra_admit(' "$trace_asm" | cut -d: -f1)
marker_line=$(grep -n 'trace_arm64_admission_rel(T, TRACE_ARM64_INT_FORL_ADMITTED);' \
  "$trace_asm" | cut -d: -f1)
test -n "$postra_line" && test -n "$marker_line" &&
test "$postra_line" -lt "$marker_line"
if grep -F 'T->spadjust != 0 || as->evenspill' "$trace_asm" >/dev/null; then
  echo "ARM64 assembly gate still keys spill admission to allocator cursors" >&2
  exit 1
fi

# Scalar constants, guards, snapshot partitioning and physical spill limits
# are exercised by independent semantic/post-RA mutations in the C fixture.
for required in \
  'test_scalar_capabilities(J);' \
  'test_postra_spill_layout(J);' \
  'test_numeric_postra_layout(J);' \
  'test_dynamic_forl_positive_and_negative(J);' \
  'test_dynamic_forl_postra_layout(J);' \
  'test_snapshot_payload_rejections(J);' \
  'test_start_metadata_rejections(J);' \
  'test_phi_and_xpoll_rejections(J);' \
  'test_deterministic_trace_error(L, J);'; do
  grep -F "$required" "$root/tests/t-arm64-jit-ir-admission.c" >/dev/null
done

# Root XPOLL observes TG-local stop/profile requests and the GC2 phase gate.
# Recorder and VM root policies remain independently gated.
test "$(grep -Fc '#if (LJ_TARGET_X64 || LJ_TARGET_ARM64) && LJ_GC64' \
  "$root/src/lj_record.c")" -eq 3
sed -n '/^static LJ_AINLINE int loop_needs_xpoll/,/^#else/p' \
  "$root/src/lj_opt_loop.c" | grep -F '#if LJ_TARGET_ARM64' >/dev/null
sed -n '/^static LJ_AINLINE int loop_needs_xpoll/,/^#else/p' \
  "$root/src/lj_opt_loop.c" | grep -F 'return 1;' >/dev/null
grep -A2 '^#elif LJ_TARGET_ARM64 && LJ_GC64' "$root/src/lj_opt_loop.c" | \
  grep -F 'emitir_raw(IRTG(IR_XPOLL, IRT_NIL), loop_needs_xpoll(J), 0);' >/dev/null
grep -F 'emit_getgl32acq(as, gate, gc2.jit_phase_gate);' \
  "$root/src/lj_asm_arm64.h" >/dev/null
grep -F 'emit_gettg32(as, gate, poll);' "$root/src/lj_asm_arm64.h" >/dev/null
grep -F 'emit_gettg32(as, profile, profile_request);' \
  "$root/src/lj_asm_arm64.h" >/dev/null
grep -A24 '^void LJ_FASTCALL lj_trace_hot' "$root/src/lj_trace.c" | \
  grep -F '#if LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED' >/dev/null
grep -A10 '^void lj_trace_ins' "$root/src/lj_trace.c" | \
  grep -F '#if LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED' >/dev/null
grep -F '#if LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED' \
  "$root/src/vm_arm64.dasc" >/dev/null

# Fixed-size CNEW must use the same publish-safe helper as the x64 backend.
# The upstream raw allocator leaves a lockless arena allocation in CONSTRUCT
# after the generated stores initialize its header.
awk '/^static void asm_cnew/ { copying = 1 }
     copying { print }
     copying && /^\/\* -- Write barriers/ { exit }' \
  "$root/src/lj_asm_arm64.h" >"$arm64_cnew_region"
for required in \
  'lj_ctype_info_predefined(cts, id, &info, &sz, NULL, NULL)' \
  'lj_ctype_info_snapshot(cts, id, &info, &sz, NULL, NULL)' \
  'IRCALL_lj_cdata_new_forjit' \
  'args[1] = ir->op1;' \
  'args[2] = ASMREF_TMP1;'; do
  grep -F "$required" "$arm64_cnew_region" >/dev/null || {
    echo "ARM64 publish-safe CNEW path changed: $required" >&2
    exit 1
  }
done
if grep -F 'IRCALL_lj_mem_newgco' "$arm64_cnew_region" >/dev/null; then
  echo "ARM64 CNEW regressed to the unpublished raw allocator" >&2
  exit 1
fi

# arm64e/BTI compilation catches pointer-auth and branch-tracking target drift
# without executing any admitted trace. Verify the intended ABI feature macros
# first, so unsupported or misspelled flags cannot silently weaken this audit.
# shellcheck disable=SC2086 # pauth_xcflags intentionally expands to arguments.
"$cc" -arch arm64e -mbranch-protection=bti \
  -mmacosx-version-min="$minver" $pauth_xcflags -I"$root/src" \
  -x c -dM -E -include lj_arch.h /dev/null >"$pauth_macros"
for required in \
  '#define LJ_TARGET_ARM64 1' \
  '#define LJ_ABI_PAUTH 1' \
  '#define LJ_ABI_BRANCH_TRACK 1'; do
  grep -F "$required" "$pauth_macros" >/dev/null || {
    echo "ARM64e/BTI audit macro missing: $required" >&2
    exit 1
  }
done

# shellcheck disable=SC2086 # pauth_xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O0 -Wall -Wextra -Werror -arch arm64e \
  -mbranch-protection=bti -mmacosx-version-min="$minver" \
  $pauth_xcflags -I"$root/src" \
  -c "$asm_source" -o "$audit_object"

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  "$root/tests/t-arm64-jit-ir-admission.c" "$archive" -lm -pthread \
  -o "$fixture"
"$fixture"

echo "arm64_jit_ir_admission_contract OK: generic scalar capabilities, independent semantic/post-RA corruption checks, exact INT FORL, publication ordering and ARM64e/BTI compilation"
