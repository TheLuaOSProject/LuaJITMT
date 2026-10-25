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

scalar_semantic=$tmpdir/scalar-semantic.txt
scalar_postra=$tmpdir/scalar-postra.txt
semantic_tablen_call=$tmpdir/semantic-tablen-call.txt
postra_tablen_call=$tmpdir/postra-tablen-call.txt
awk '/^static int arm64_scalar_ir_admit/ { copying = 1 }
     copying && /^static int arm64_scalar_postra_kref/ { exit }
     copying { print }' "$admit_source" >"$scalar_semantic"
awk '/^static int arm64_scalar_postra_kref/ { copying = 1 }
     copying && /^int lj_asm_arm64_ir_admit/ { exit }
     copying { print }' "$admit_source" >"$scalar_postra"
test -s "$scalar_semantic" && test -s "$scalar_postra"
awk '/case IR_CALLS:/ { copying = 1 }
     copying && /^    case IR_CALLXS:/ { exit }
     copying { print }' "$scalar_semantic" >"$semantic_tablen_call"
awk '/case IR_CALLS:/ { copying = 1 }
     copying && /^    case IR_ADDOV:/ { exit }
     copying { print }' "$scalar_postra" >"$postra_tablen_call"
test -s "$semantic_tablen_call" && test -s "$postra_tablen_call"

# The two certificates must derive their own decisions from their inputs.
# Basic checked bytecode range utilities may be shared; verdicts and scalar
# provenance checks may not be shared between semantic and immutable views.
if grep -F 'arm64_scalar_postra_' "$scalar_semantic" >/dev/null ||
   grep -F 'arm64_scalar_ir_' "$scalar_postra" >/dev/null; then
  echo "ARM64 scalar certificates share an admission decision" >&2
  exit 1
fi
for region in "$scalar_semantic" "$scalar_postra"; do
  for opcode in SLOAD ADDOV SUBOV MULOV ADD SUB MUL DIV CONV LT GE LE GT EQ NE LOOP XPOLL PHI; do
    grep -F "case IR_$opcode:" "$region" >/dev/null || {
      echo "ARM64 scalar capability missing: IR_$opcode" >&2
      exit 1
    }
  done
  for forbidden in AREF HREF UREFO UREFC FLOAD XLOAD ASTORE HSTORE USTORE FSTORE XSTORE SNEW TNEW CNEW BUFHDR TBAR OBAR XBAR XSAVE RETF PROF CARG MOD POW FPMATH; do
    if grep -E "case[[:space:]]+IR_$forbidden:" "$region" >/dev/null; then
      echo "unsupported scalar capability gained a case: IR_$forbidden" >&2
      exit 1
    fi
  done
  if grep -E 'lj_mcode_|trace_save|traceslot_publish|lj_ir_call|asm_call|lj_trace_err' "$region" >/dev/null; then
    echo "ARM64 scalar validator acquired allocation/publication side effects" >&2
    exit 1
  fi
done

# General native calls remain rejected. IR_CALLS has one isolated exception:
# the rooted-length profile audited below; its exact callee/root/guard checks
# must not weaken blanket CALLN/CALLA/CALLL/CALLXS rejection.
awk '/case IR_CALLN:/ { copying = 1 }
     copying && /^    case IR_CALLS:/ { copying = 0 }
     /^    case IR_CALLXS:/ { copying = 1 }
     copying && /^    default:/ { exit }
     copying { print }' "$scalar_semantic" >"$call_region"
for opcode in CALLN CALLA CALLL CALLXS; do
  grep -F "case IR_$opcode:" "$call_region" >/dev/null || {
    echo "ARM64 general call blacklist lost IR_$opcode" >&2
    exit 1
  }
done
test "$(grep -c 'LJ_ARM64_IR_REJECT_CALL' "$call_region")" -eq 2
if grep -E 'break;|return 1|IR_CALLS[[:space:]]*:' "$call_region" >/dev/null; then
  echo "ARM64 general call gate acquired an unscoped helper" >&2
  exit 1
fi

# The sole CALLS exception is a bounded rooted table-length capability. Keep
# the semantic and post-RA certificates independent, with a checked IN1 root,
# exact helper identity, and an immediate retry-sentinel guard backed by a real
# integer constant-pool member.
for required in \
  'static int arm64_tablen_retry_kint(const IRIns *ir, IRRef nk, IRRef ref)' \
  'ref < nk || ref >= REF_TRUE' \
  'k.o == IR_KINT && k.t.irt == IRT_INT' \
  'k.i == LJ_TAB_LEN_RETRY' \
  'IRCALL_lj_tab_len_rooted_try' \
  'tablen_table >= tablen_root' \
  'tablen_guard != tablen_calls' \
  'guard != call + 1u' \
  'guard != ref + 1u' \
  'arm64_tablen_retry_kint(T->ir, T->nk, kref)' \
  'arm64_tablen_retry_kint(v->ir, v->nk, kref)' \
  'LJ_ARM64_TABLEN_MAX_CALLS 8u' \
  'IRTMPREF_IN1'; do
  grep -F "$required" "$admit_source" >/dev/null || {
    echo "ARM64 rooted-length admission proof changed: $required" >&2
    exit 1
  }
done
for required in \
  'ir->op1 < REF_FIRST || ir->op1 >= ref' \
  'T->ir[ir->op1].o != IR_TMPREF' \
  'T->ir[ir->op1].op1 != tablen_table' \
  'ir->op2 != IRCALL_lj_tab_len_rooted_try' \
  'tablen_calls++'; do
  grep -F "$required" "$semantic_tablen_call" >/dev/null || {
    echo "ARM64 semantic rooted-length call proof lost $required" >&2
    exit 1
  }
done
for required in \
  'p.op1 < REF_FIRST || p.op1 >= ref' \
  'ir_load_acq(&v->ir[p.op1]).o != IR_TMPREF' \
  'ir_load_acq(&v->ir[p.op1]).op1 != tablen_table' \
  'p.op2 != IRCALL_lj_tab_len_rooted_try' \
  'tablen_calls++'; do
  grep -F "$required" "$postra_tablen_call" >/dev/null || {
    echo "ARM64 post-RA rooted-length call proof lost $required" >&2
    exit 1
  }
done
for required in \
  'p.op2 != IRTMPREF_IN1' \
  'guard != ref + 1u'; do
  grep -F "$required" "$scalar_postra" >/dev/null || {
    echo "ARM64 post-RA rooted-length retry proof lost $required" >&2
    exit 1
  }
done
# Keep the capability tied to its independent runtime/negative contract and
# user-visible active-MT IR assertion. That companion builds and executes both
# ARM64 and ARM64e/BTI profiles.
grep -F 'sh "$root/tools/ci/arm64_jit_rooted_tablen_contract.sh"' \
  "$root/tools/ci/arm64_jit_fail_closed_gate.sh" >/dev/null || {
  echo 'ARM64 fail-closed gate no longer enrolls the rooted-length contract' >&2
  exit 1
}
for required in \
  'fixture_source=$root/tests/t-tab-rooted-len-try.c' \
  "build_and_run arm64 '-arch arm64'" \
  "build_and_run arm64e '-arch arm64e -mbranch-protection=bti'"; do
  grep -F "$required" "$root/tools/ci/arm64_jit_rooted_tablen_contract.sh" \
    >/dev/null || {
    echo "ARM64 rooted-length companion contract lost: $required" >&2
    exit 1
  }
done
for required in \
  'assert(!lj_asm_arm64_ir_admit(fake, &semantic, &reject));' \
  'assert(!lj_asm_arm64_postra_admit(&v, NULL));' \
  'IRCALL_lj_tab_len' \
  'LJ_TAB_LEN_RETRY + 1' \
  'IRTMPREF_IN1 | IRTMPREF_IN2' \
  'guard != call + 1u' \
  'LJ_TAB_LEN_RETRY'; do
  grep -F "$required" "$root/tests/t-tab-rooted-len-try.c" \
    "$admit_source" >/dev/null || {
    echo "ARM64 rooted-length independent negative missing: $required" >&2
    exit 1
  }
done
grep -F 'assert_rooted_len_ir("active-MT #table BC_LOOP")' \
  "$root/tests/suites/m6_jit.lua" >/dev/null || {
  echo 'ARM64 active-MT rooted-length native IR assertion was removed' >&2
  exit 1
}

# Generic scalar checks coexist with independently scoped FORL, FUNCF and
# CALLXS admission. They must not absorb these roots or native-call layouts.
for required in \
  'static int arm64_ir_forl_shape(' \
  'static int arm64_ir_funcf_shape(' \
  'static int arm64_ir_callxs_shape(' \
  'static int arm64_postra_forl_dynamic_shape(' \
  'static int arm64_postra_funcf_admit(' \
  'static int arm64_postra_callxs_admit('; do
  grep -F "$required" "$admit_source" >/dev/null
done
if grep -E 'ARM64_NUMDYN_|ARM64_NUMARG_|numacc_shape|numadd_shape' "$admit_source" >/dev/null; then
  echo "obsolete exact numeric trace profiles remain in scalar admission" >&2
  exit 1
fi

awk '/^static void make_trace\(/ { copying = 1 }
     copying { print }
     copying && /^static void make_forl_trace\(/ { exit }' \
  "$root/tests/t-arm64-jit-ir-admission.c" >"$positive_region"
test "$(grep -c 'IR_SLOAD' "$positive_region")" -eq 3
test "$(grep -c 'IR_SUBOV' "$positive_region")" -eq 2
test "$(grep -c 'IR_MULOV' "$positive_region")" -eq 2
test "$(grep -c 'IR_GE' "$positive_region")" -eq 1
test "$(grep -c 'IR_LE' "$positive_region")" -eq 1
test "$(grep -c 'IR_PHI' "$positive_region")" -eq 2
if grep -E 'IR_(KNUM|NUM|CONV|ADD,|SUB,|MUL,|DIV|USE)' \
     "$positive_region" >/dev/null; then
  echo "integer fixture unexpectedly acquired numeric operations" >&2
  exit 1
fi

# The variable-stop/variable-step FORL fixture pins the recorder's exact
# direction and overflow proof, including the sole scoped IR_USE.
awk '/^static void make_dynamic_forl_trace/ { copying = 1 }
     copying { print }
     copying && /^static void make_numeric_trace/ { exit }' \
  "$root/tests/t-arm64-jit-ir-admission.c" >"$dynamic_forl_region"
for required in \
  'V_R_STOP, IR_SLOAD, IRT_INT, idxslot+FORL_STOP' \
  'V_R_STEP, IR_SLOAD, IRT_INT, idxslot+FORL_STEP' \
  'V_R_DIRECTION, direction, IRT_INT|IRT_GUARD' \
  'V_R_OVERFLOW, IR_ADDOV, IRT_INT|IRT_GUARD' \
  'V_R_USE, IR_USE, IRT_INT, V_R_OVERFLOW, 0' \
  'V_R_IDX_PRE, IR_ADD, IRT_INT|IRT_ISPHI' \
  'V_R_IDX_BODY, IR_ADD, IRT_INT|IRT_ISPHI' \
  'SNAP(8, SNAP_NORESTORE, V_R_STEP)' \
  'fx.T.nsnapmap = 32;' \
  'fx.T.nins = V_R_SEMANTIC_END;'; do
  grep -F "$required" "$dynamic_forl_region" >/dev/null || {
    echo "ARM64 dynamic FORL fixture changed: $required" >&2
    exit 1
  }
done
test "$(grep -c 'IR_USE' "$dynamic_forl_region")" -eq 1

awk '/^static void test_dynamic_forl_positive_and_negative/ { copying = 1 }
     copying { print }
     copying && /^static void test_postra_spill_layout/ { exit }' \
  "$root/tests/t-arm64-jit-ir-admission.c" >"$dynamic_forl_tests"
for required in \
  'fx.ir[V_R_STEP].op2 = IRSLOAD_TYPECHECK|IRSLOAD_INHERIT;' \
  'fx.ir[V_R_DIRECTION].o = IR_GT;' \
  'fx.ir[V_R_OVERFLOW].op1 = V_R_STOP;' \
  'fx.ir[V_R_USE].op1 = V_R_STOP;' \
  'fx.ir[V_R_USE].s = SPS_FIRST;' \
  'fx.ir[V_R_OVERFLOW].r = RID_INIT;' \
  'fx.ir[V_R_STEP].r = RID_D0;' \
  'fx.ir[V_R_STEP].r = fx.ir[V_R_STOP].r;' \
  'fx.ir[V_R_IDX].r = fx.ir[V_R_STEP].r;' \
  'fx.ir[V_R_SUM].r = fx.ir[V_R_IDX].r;' \
  'fx.ir[V_R_OVERFLOW].r = fx.ir[V_R_STEP].r;' \
  'fx.ir[V_R_IDX_BODY].r = RID_X26;' \
  'fx.ir[V_R_SUM_BODY].r = RID_X26;' \
  'fx.ir[V_R_SUM_PHI].r = fx.ir[V_R_IDX_PHI].r;' \
  'fx.snapmap[4] = SNAP(8, SNAP_NORESTORE, V_R_STOP);'; do
  grep -F "$required" "$dynamic_forl_tests" >/dev/null || {
    echo "ARM64 dynamic FORL mutation coverage changed: $required" >&2
    exit 1
  }
done
for required in \
  'test_dynamic_forl_positive_and_negative(J);' \
  'test_dynamic_forl_postra_layout(J);'; do
  grep -F "$required" "$root/tests/t-arm64-jit-ir-admission.c" >/dev/null
done

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
