#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_ir_admission_contract SKIP: requires native macOS arm64"
  exit 0
fi

cc=${CC:-clang}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
archive=$root/src/libluajit.a
asm_object=$root/src/lj_asm.o
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-ir-admission.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

fixture=$tmpdir/t-arm64-jit-ir-admission
classifier=$tmpdir/classifier.txt
trace_asm=$tmpdir/trace-asm.txt
call_region=$tmpdir/call-region.txt
value_region=$tmpdir/value-region.txt
positive_region=$tmpdir/positive-region.txt
audit_object=$tmpdir/lj_asm-arm64e.o
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT'

test -f "$archive" && test -f "$asm_object" || {
  echo "ARM64 IR admission contract requires an existing experimental build" >&2
  exit 1
}

nm "$archive" | grep ' T _lj_asm_arm64_ir_admit$' >/dev/null || {
  echo "experimental archive lacks the ARM64 IR admission gate" >&2
  exit 1
}

awk '/^\/\* -- Initial ARM64 IR admission/ { copying = 1 }
     copying { print }
     copying && /^\/\* -- Assembler state and common macros/ { exit }' \
  "$root/src/lj_asm.c" >"$classifier"
test -s "$classifier"

awk '/^static int arm64_ir_int_value_op/ { copying = 1 }
     copying { print }
     copying && /^static int arm64_ir_int_ref/ { exit }' \
  "$root/src/lj_asm.c" >"$value_region"
grep -F 'case IR_SLOAD: case IR_ADDOV: case IR_SUBOV: case IR_MULOV:' \
  "$value_region" >/dev/null
if grep -E 'case IR_(CONV|ADD|SUB|MUL|DIV|LT|GE|LE|GT|EQ|NE|USE|PHI|LOOP|XPOLL):' \
     "$value_region" >/dev/null; then
  echo "non-value IR entered the ARM64 integer producer set" >&2
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
  echo "removed numeric family entered the integer positive fixture" >&2
  exit 1
fi

# Keep the production check ahead of IR scratch growth, compact-trace
# allocation and mcode reservation/publication.
awk '/^void lj_asm_trace\(/ { copying = 1 }
     copying { print }
     copying && /^#if LJ_TARGET_ARM64 && defined\(LJ_ARM64_EMIT_TEST_HELPERS\)/ {
       exit
     }' "$root/src/lj_asm.c" >"$trace_asm"
admit_line=$(grep -n 'lj_asm_arm64_ir_admit(J, T, &reject)' "$trace_asm" | cut -d: -f1)
nextins_line=$(grep -n 'as->orignins = lj_ir_nextins(J)' "$trace_asm" | cut -d: -f1)
reserve_line=$(grep -n 'lj_mcode_reserve(J, &as->mcbot)' "$trace_asm" | cut -d: -f1)
test -n "$admit_line" && test "$admit_line" -lt "$nextins_line" &&
test "$admit_line" -lt "$reserve_line"
grep -F 'lj_trace_err_info(J, LJ_TRERR_NYIIR);' "$trace_asm" >/dev/null
for required in \
  'finalview.nins = arm64_semantic_nins;' \
  'T->spadjust != 0 || as->evenspill != SPS_FIRST || as->oddspill != 0' \
  'if (ra_hasspill(ir.s))' \
  'if (irref_isk(snapref) || (sn & SNAP_FRAME))' \
  'rs = ir_load_acq(&finalir[snapref]).prev;' \
  'for (renref = finalnins; renref-- > arm64_semantic_nins; )' \
  'if (ren.op1 == snapref && ren.op2 <= snapno)' \
  'if (ra_hasspill(regsp_spill(rs)) ||' \
  'regsp_reg(rs) >= RID_MAX_GPR ||' \
  '!rset_test(RSET_GPR, regsp_reg(rs)))' \
  'finalnins == arm64_semantic_nins + 1u' \
  'ir.o == IR_NOP && ir.t.irt == IRT_NIL' \
  'if (!suffix_ok && finalnins > arm64_semantic_nins &&' \
  'finalnins - arm64_semantic_nins <= LJ_MAX_PHI' \
  'ir.o != IR_RENAME || ir.t.irt != IRT_NIL' \
  'ra_hasspill(ir.s)' \
  'T->unused1 |= TRACE_ARM64_INT_LOOP_ADMITTED;'; do
  grep -F "$required" "$trace_asm" >/dev/null || {
    echo "ARM64 post-RA admission check changed: $required" >&2
    exit 1
  }
done
test "$(grep -Fc 'ir.r >= RID_MAX_GPR' "$trace_asm")" -eq 1
if grep -E 'rset_test\(RSET_GPR, ir\.r\).*ir\.r >= RID_MAX_GPR' \
     "$trace_asm" >/dev/null; then
  echo "ARM64 post-RA register range check follows rset_test" >&2
  exit 1
fi
snap_range_line=$(grep -n 'regsp_reg(rs) >= RID_MAX_GPR' "$trace_asm" | cut -d: -f1)
snap_rset_line=$(grep -n '!rset_test(RSET_GPR, regsp_reg(rs))' "$trace_asm" | cut -d: -f1)
snap_spill_line=$(grep -n 'if (ra_hasspill(regsp_spill(rs)) ||' "$trace_asm" | cut -d: -f1)
suffix_line=$(grep -n 'if (LJ_UNLIKELY(!suffix_ok))' "$trace_asm" | cut -d: -f1)
snap_loop_line=$(grep -n 'for (snapno = 0; snapno < T->nsnap; snapno++)' "$trace_asm" | cut -d: -f1)
marker_line=$(grep -n 'T->unused1 |= TRACE_ARM64_INT_LOOP_ADMITTED;' "$trace_asm" | cut -d: -f1)
rename_range_line=$(grep -n 'ir.op2 >= T->nsnap || ir.r >= RID_MAX_GPR ||' "$trace_asm" | cut -d: -f1)
rename_rset_line=$(grep -n '!rset_test(RSET_GPR, ir.r)' "$trace_asm" | cut -d: -f1)
test -n "$snap_spill_line" && test -n "$snap_range_line" &&
test -n "$snap_rset_line" && test "$snap_spill_line" -lt "$snap_range_line" &&
test "$snap_range_line" -lt "$snap_rset_line"
test -n "$suffix_line" && test -n "$snap_loop_line" &&
test -n "$marker_line" && test "$suffix_line" -lt "$snap_loop_line" &&
test "$snap_loop_line" -lt "$marker_line"
test -n "$rename_range_line" && test -n "$rename_rset_line" &&
test "$rename_range_line" -lt "$rename_rset_line"
if grep -F '} else if (finalnins > arm64_semantic_nins' \
     "$trace_asm" >/dev/null; then
  echo "one allocator RENAME is shadowed by the spare-NOP branch" >&2
  exit 1
fi

if grep -E 'lj_mcode_|trace_save|traceslot_publish|lj_ir_call|asm_call' \
     "$classifier" >/dev/null; then
  echo "ARM64 IR classifier acquired code/publication/helper side effects" >&2
  exit 1
fi

# Exact candidate opcode inventory. CALL opcodes appear only so they can report
# a helper ID and reject it; the admitted helper-ID set remains empty.
for cases in \
  'case IR_BASE:' \
  'case IR_SLOAD:' \
  'case IR_LT: case IR_GE: case IR_LE: case IR_GT:' \
  'case IR_EQ: case IR_NE:' \
  'case IR_ADDOV: case IR_SUBOV: case IR_MULOV:' \
  'case IR_PHI:' \
  'case IR_LOOP:' \
  'case IR_XPOLL:' \
  'case IR_CALLN: case IR_CALLA: case IR_CALLL: case IR_CALLS:' \
  'case IR_CALLXS:'; do
  grep -F "$cases" "$classifier" >/dev/null || {
    echo "ARM64 IR classifier inventory changed: $cases" >&2
    exit 1
  }
done

awk '/case IR_CALLN:/ { copying = 1 }
     copying { print }
     copying && /default:/ { exit }' "$classifier" >"$call_region"
test "$(grep -c 'LJ_ARM64_IR_REJECT_CALL' "$call_region")" -eq 2
if grep -E 'break;|return 1|IRCALL_[A-Za-z0-9_]+[[:space:]]*:' \
     "$call_region" >/dev/null; then
  echo "ARM64 CALL helper allowlist is no longer empty" >&2
  exit 1
fi

for forbidden in IR_KGC IR_KPTR IR_KKPTR IR_KNULL IR_KINT64 IR_KSLOT \
  IR_NOP IR_CONV IR_ADD IR_SUB IR_MUL IR_DIV IR_ULT IR_UGE IR_ULE IR_UGT \
  IR_USE IR_NEG IR_MOD IR_POW \
  IR_ABS IR_LDEXP \
  IR_MIN IR_MAX IR_FPMATH \
  IR_AREF IR_HREF IR_UREFO IR_FLOAD IR_XLOAD IR_ASTORE IR_HSTORE \
  IR_USTORE IR_FSTORE IR_XSTORE IR_SNEW IR_TNEW IR_CNEW IR_BUFHDR \
  IR_TBAR IR_OBAR IR_XBAR IR_XSAVE IR_RETF IR_PROF IR_CARG; do
  if grep -E "case[[:space:]]+$forbidden:" "$classifier" >/dev/null; then
    echo "forbidden ARM64 IR unexpectedly gained an admitted case: $forbidden" >&2
    exit 1
  fi
done

for required in \
  'T->sinktags != 0' \
  'for (ref = REF_TRUE; ref <= REF_NIL; ref++)' \
  'ir->o != IR_KPRI || ir->t.irt != expected || ir->op12 != 0' \
  'ir->o != IR_KINT || ir->t.irt != IRT_INT' \
  'arm64_ir_int_value_op((IROp)ir->o)' \
  'arm64_ir_proto_range(pt, &lo, &hi)' \
  'startpc == NULL ||' \
  'startpc != J->startpc' \
  'arm64_ir_bc_acq(lo, pos) != startins' \
  'bc_op(back) != BC_JMP' \
  'target > (int64_t)pos' \
  '(MSize)slot > (MSize)pt->framesize' \
  'ir->op1 < 1 + LJ_FR2 || ir->op1 >= maxslots ||' \
  'ir->op1 >= root_topslot + 1u + LJ_FR2' \
  'ir->op2 != IRSLOAD_TYPECHECK' \
  'ref >= before' \
  'mapofs > T->nsnapmap || nextofs > T->nsnapmap' \
  'mapofs != expected_mapofs' \
  'nextofs - mapofs - nent != 1u + LJ_FR2' \
  'slot >= nslots' \
  'slot <= snap_slot(T->snapmap[mapofs+n-1])' \
  'uint32_t flags = sn & 0x00ff0000u;' \
  'slot < 1 + LJ_FR2 || flags != 0' \
  '(uint8_t)pcbase != (uint8_t)(J->baseslot-2)' \
  '!arm64_ir_pcpos(snappc, proto_lo, proto_hi, &snappos)' \
  'topslot != root_topslot' \
  'nslots > root_topslot + 1u + LJ_FR2' \
  'T->snap[0].ref > ref' \
  'xpollref != loopref + 1u' \
  'xpollsnap != loopref' \
  'ir->op1 != 1' \
  'J->loopref != loopref' \
  'firstphi != 0 && ir->o != IR_PHI' \
  'ir->op1 < REF_FIRST || ir->op1 >= loopref' \
  'ir->op2 <= xpollref || ir->op2 >= firstphi' \
  'nphi > LJ_MAX_PHI' \
  '!irt_isphi(T->ir[ir->op1].t)' \
  '!irt_isphi(T->ir[ir->op2].t)' \
  'T->ir[prevphi].op1 == ir->op1' \
  '!!irt_isphi(T->ir[ref].t) != operand' \
  'startop != BC_LOOP'; do
  grep -F "$required" "$classifier" >/dev/null || {
    echo "ARM64 IR structural check changed: $required" >&2
    exit 1
  }
done

# The synthetic admission fixture exercises real prototype bytecode and each
# structural corruption which must remain fail-closed before publication.
for required in \
  'fixture_snapshot_pc = fixture_forl_pc + 1 + bc_j(loadbc(fixture_forl_pc));' \
  'set_snapshot_payload(2, fixture_snapshot_pc, 1);' \
  'set_snapshot_payload(2, (const BCIns *)(lo+1), 0);' \
  'set_snapshot_payload(2, (const BCIns *)(lo-sizeof(BCIns)), 0);' \
  'set_snapshot_payload(2, (const BCIns *)hi, 0);' \
  'setmref(fx.T.startpc, fixture_snapshot_pc);' \
  'fx.T.startins = BCINS_AJ(BC_LOOP, bc_a(loop)^1u, bc_j(loop));' \
  'fixture_pt->sizebc = 0;' \
  'fixture_pt->sizebc = (MSize)(fixture_loop_pc-bc);' \
  'make_forl_trace(J);' \
  'fx.T.startins = loadbc(fixture_forl_pc);' \
  'setir(REF_TRUE, IR_KINT, IRT_INT, 1, 0);' \
  'setir(REF_FALSE, IR_KNUM, IRT_NUM, 0, 0);' \
  'setir(K_STOP, IR_KNUM, IRT_NUM, 0, 0);' \
  'fixture_loop_pc-loopbackpc));' \
  'BCINS_AJ(BC_LOOP, fixture_pt->framesize+1u, bc_j(loop))' \
  'fx.ir[R_PHI2].op1 = R_SUM1;' \
  'LJ_STATIC_ASSERT(REF_FIRST + 3*LJ_MAX_PHI + 2u <= ADMISSION_IR_CAP);' \
  'sizeof(fx.ir)/sizeof(fx.ir[0])' \
  'phi = post + LJ_MAX_PHI + 1u;' \
  'fx.T.nins = end;' \
  'fx.snap[3].ref = R_XPOLL;' \
  'fx.snapmap[6] = SNAP(2, SNAP_NORESTORE, R_SUM1);' \
  'fx.ir[R_BODY1].op1 = R_PRECOND;' \
  'REJECT_REMOVED(R_SUM1, IR_USE, IRT_INT, R_A, 0);' \
  'IR_LT, IR_GE, IR_LE, IR_GT, IR_EQ, IR_NE' \
  'IR_ADDOV, IR_SUBOV, IR_MULOV' \
  'IR_ULT, IR_UGE, IR_ULE, IR_UGT' \
  'expect_reject(J, LJ_ARM64_IR_REJECT_OPCODE, unsigned_guards[guardno]);' \
  'setir(R_SUM1, IR_ADDOV, IRT_INT|IRT_ISPHI, R_A, R_B);' \
  'fx.ir[R_SUM2].t.irt &= (uint8_t)~IRT_GUARD;' \
  'expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_MULOV);' \
  'fx.snap[0].mapofs = fx.T.nsnapmap+1u;' \
  'fx.snap[1].mapofs = fx.T.nsnapmap+1u;' \
  'fx.ir[R_A].t.irt = IRT_NUM|IRT_GUARD;'; do
  grep -F "$required" "$root/tests/t-arm64-jit-ir-admission.c" >/dev/null || {
    echo "ARM64 IR negative fixture coverage changed: $required" >&2
    exit 1
  }
done

# ARM64 records the already-lowered full XPOLL only for root LOOP traces.
# Side/stitch recording and non-loop native entry remain independently closed.
test "$(grep -Fc '#if (LJ_TARGET_X64 || LJ_TARGET_ARM64) && LJ_GC64' \
  "$root/src/lj_record.c")" -eq 3
test "$(grep -Fc '#if (LJ_TARGET_X64 || LJ_TARGET_ARM64) && LJ_GC64' \
  "$root/src/lj_opt_loop.c")" -eq 2
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

# arm64e/BTI compilation catches pointer-auth target drift without executing
# any admitted trace.
# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O0 -Wall -Wextra -Werror -arch arm64e \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  -c "$root/src/lj_asm.c" -o "$audit_object"

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  "$root/tests/t-arm64-jit-ir-admission.c" "$archive" -lm -pthread \
  -o "$fixture"
"$fixture"

echo "arm64_jit_ir_admission_contract OK: exact spill-free scalar BC_LOOP policy and fail-closed boundary verified"
