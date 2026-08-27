#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_side_ir_admission_contract SKIP: requires native macOS arm64"
  exit 0
fi

if test -z "${SDKROOT:-}"; then
  SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
  export SDKROOT
fi

cc=${CC:-$(xcrun --sdk macosx --find clang)}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
archive=$root/src/libluajit.a
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-side-ir.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

fixture=$tmpdir/t-arm64-jit-side-ir-admission
audit_object=$tmpdir/lj_asm-arm64e.o
pure_region=$tmpdir/pure-side-region.txt
trace_asm=$tmpdir/trace-asm.txt
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT'

test -f "$archive" || {
  echo "ARM64 side IR contract requires an existing experimental build" >&2
  exit 1
}

nm "$archive" | grep ' T _lj_asm_arm64_side_ir_admit$' >/dev/null || {
  echo "experimental archive lacks the pure ARM64 side semantic gate" >&2
  exit 1
}
nm "$archive" | grep ' T _lj_asm_arm64_side_postra_admit$' >/dev/null || {
  echo "experimental archive lacks the pure ARM64 side post-RA gate" >&2
  exit 1
}

# This tranche reserves policy and tests only. Production recording, dispatch
# and marker publication remain independently closed.
grep -E '^#define LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED[[:space:]]+1$' \
  "$root/src/lj_arch.h" >/dev/null
grep -F '#define TRACE_ARM64_INT_SIDE_ADMITTED' "$root/src/lj_jit.h" | \
  grep -F '0x80' >/dev/null
grep -F 'lj_asm_arm64_side_ir_admit' "$root/src/lj_asm.h" >/dev/null
grep -F 'lj_asm_arm64_side_postra_admit' "$root/src/lj_asm.h" >/dev/null
grep -F 'sh "$root/tools/ci/arm64_jit_side_ir_admission_contract.sh"' \
  "$root/tools/ci/arm64_jit_fail_closed_gate.sh" >/dev/null

awk '/^\/\* -- Pure ARM64 first-side admission/ { copy=1 }
     copy { print }
     copy && /^static int arm64_ir_funcf_snapshots/ { exit }' \
  "$root/src/lj_asm.c" >"$pure_region"
test -s "$pure_region"

for required in \
  'ARM64_SIDE_K_ONE = REF_TRUE-1u' \
  'ARM64_SIDE_SEMANTIC_NINS = REF_BASE+7u' \
  'static const IRRef snaprefs[4]' \
  'static const MSize mapofs[4] = { 0, 3, 7, 10 };' \
  'static const uint8_t nent[4] = { 1, 2, 1, 1 };' \
  'static const uint8_t nslots[4] = { 5, 6, 5, 5 };' \
  'static const MSize pcpos[4] = { 13, 3, 17, 7 };' \
  'view->proto_sizebc != 19u' \
  'view->traceno == 0' \
  'view->traceno > UINT16_MAX || view->parent == 0' \
  'view->parent > UINT16_MAX || view->traceno == view->parent' \
  'view->root != view->parent' \
  'view->link != view->parent || view->exitno != 2u' \
  'view->startins != BCINS_AD(BC_JMP, 0, 0)' \
  'view->linktype != LJ_TRLINK_ROOT' \
  'ins.o != IR_KINT || ins.t.irt != IRT_INT || ins.i != 1' \
  'ARM64_SIDE_REQUIRE(REF_BASE, IR_BASE, IRT_PGC,' \
  'view->parent, view->exitno);' \
  'IRSLOAD_PARENT|IRSLOAD_INHERIT' \
  'ARM64_SIDE_R_ADD, IR_ADDOV, IRT_INT|IRT_GUARD' \
  'ARM64_SIDE_R_GT, IR_GT, IRT_INT|IRT_GUARD' \
  'ARM64_SIDE_R_LIMIT, ARM64_SIDE_R_ADD);' \
  'ARM64_SIDE_R_XPOLL, IR_XPOLL, IRT_NIL|IRT_GUARD, 1, 0' \
  'expected > (uintptr_t)(UINT64_MAX >> 8)' \
  'SNAP(4, 0, ARM64_SIDE_R_PARENT)' \
  'SNAP(5, 0, ARM64_SIDE_R_ADD)' \
  'view->nins != ARM64_SIDE_SEMANTIC_NINS+1u' \
  'view->spadjust != 0 || view->parent_spadjust != 0' \
  'ins.o != IR_NOP || ins.t.irt != IRT_NIL' \
  '!regsp_used(parentrs) || ra_hasspill(regsp_spill(parentrs))' \
  'ins.r != RID_BASE || ins.s != SPS_NONE' \
  'if (!arm64_side_postra_gpr(ins))' \
  'ins.r != regsp_reg(parentrs)' \
  'ins.r != RID_INIT || ins.s != SPS_NONE' \
  'ins.prev != REGSP_INIT'; do
  grep -F "$required" "$pure_region" >/dev/null || {
    echo "ARM64 side certificate invariant changed: $required" >&2
    exit 1
  }
done

if grep -E 'snap_count_acq|trace_save|traceslot_publish|lj_mcode_|lj_ir_call|asm_call|lj_trace_err|trace_exittarget' \
     "$pure_region" >/dev/null; then
  echo "pure ARM64 side certificate gained mutable-count or publication effects" >&2
  exit 1
fi

awk '/^void lj_asm_trace\(/ { copy=1 }
     copy { print }
     copy && /^#if LJ_TARGET_ARM64 && defined\(LJ_ARM64_EMIT_TEST_HELPERS\)/ {
       exit
     }' "$root/src/lj_asm.c" >"$trace_asm"
if grep -E 'lj_asm_arm64_side_(ir|postra)_admit|TRACE_ARM64_INT_SIDE_ADMITTED' \
     "$trace_asm" >/dev/null; then
  echo "Stage 1 side certificate was wired into production assembly" >&2
  exit 1
fi
if grep -F 'TRACE_ARM64_INT_SIDE_ADMITTED' "$root/src/lj_asm.c" >/dev/null; then
  echo "Stage 1 assembler unexpectedly publishes the side admission marker" >&2
  exit 1
fi

for required in \
  'fx.view.traceno = 9;' \
  'fx.view.parent = fx.view.root = fx.view.link = 7;' \
  'fx.ir[REF_BASE].op1 = 7;' \
  'fx.view.traceno = UINT16_MAX;' \
  'fx.view.parent = fx.view.root = fx.view.link = UINT16_MAX-1u;' \
  'BCIns proto[19];' \
  'fx.view.proto_sizebc = 19;' \
  'fx.view.traceno = fx.view.parent' \
  'fx.view.exitno++' \
  'setir(K_ONE, IR_KINT, IRT_INT, 2, 0)' \
  'setir(K_ONE, IR_KNUM, IRT_NUM, 1, 0)' \
  'fx.ir[R_PARENT].op2 = IRSLOAD_PARENT' \
  'fx.ir[R_VALUE].op2 |= IRSLOAD_PARENT' \
  'fx.ir[R_ADD].op2 = R_PARENT' \
  'fx.ir[R_GT].op2 = R_PARENT' \
  'fx.ir[R_XPOLL].op1 = 0' \
  'fx.snap[i].count = (uint8_t)(31u+i*37u);' \
  'expect_semantic(1);' \
  'fx.snap[i].mcofs = (uint16_t)(100u+i);' \
  'fx.snap[i].ref++' \
  'fx.snap[i].mapofs++' \
  'fx.snap[i].nent++' \
  'fx.snap[i].nslots++' \
  'set_footer(i, pcpos[i], 1)' \
  'SNAP(4, SNAP_NORESTORE, R_PARENT)' \
  'fx.postra.spadjust = 16' \
  'fx.postra.parent_spadjust = 16' \
  'setir(R_END, IR_RENAME, IRT_NIL, R_ADD, 0)' \
  'fx.postra.parent_slot4 = REGSP(RID_X4, 2)' \
  'fx.postra.parent_slot4 = REGSP(RID_D0, 0)' \
  'fx.ir[ref].r = RID_D0' \
  'fx.ir[ref].s = 2' \
  'fx.ir[R_GT].r = RID_X0' \
  'fx.ir[R_XPOLL].s = 2' \
  'TRACE_ARM64_INT_SIDE_ADMITTED == 0x80'; do
  grep -F "$required" "$root/tests/t-arm64-jit-side-ir-admission.c" >/dev/null || {
    echo "ARM64 side mutation coverage changed: $required" >&2
    exit 1
  }
done

# arm64e compilation catches target register/PAC configuration drift even
# though this pure tranche does not yet publish an authenticated child target.
# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O0 -Wall -Wextra -Werror -arch arm64e \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  -c "$root/src/lj_asm.c" -o "$audit_object"

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  "$root/tests/t-arm64-jit-side-ir-admission.c" "$archive" -lm -pthread \
  -o "$fixture"
"$fixture"

echo "arm64_jit_side_ir_admission_contract OK: pure +1 first-side semantic and synthetic post-RA certificates verified; production gate remains closed"
