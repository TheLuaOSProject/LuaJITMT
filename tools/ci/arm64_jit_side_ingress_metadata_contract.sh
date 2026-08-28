#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_side_ingress_metadata_contract SKIP: requires native macOS arm64"
  exit 0
fi

if test -z "${SDKROOT:-}"; then
  SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
  export SDKROOT
fi

cc=${CC:-$(xcrun --sdk macosx --find clang)}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
archive=$root/src/libluajit.a
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-side-ingress.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

fixture=$tmpdir/t-arm64-jit-side-ingress-metadata
fixture_arm64e=$tmpdir/t-arm64-jit-side-ingress-metadata-arm64e.o
audit_object=$tmpdir/lj_trace-arm64e.o
macros=$tmpdir/macros.txt
helper_region=$tmpdir/side-ingress-helper.txt
cert_region=$tmpdir/side-parent-cert-helper.txt
init_region=$tmpdir/side-parent-init.txt
preflight_region=$tmpdir/side-parent-preflight.txt
start_region=$tmpdir/side-parent-start.txt
downrec_region=$tmpdir/side-parent-downrec.txt
abort_region=$tmpdir/side-parent-abort.txt
abort_owner_region=$tmpdir/side-parent-abort-owner.txt
terminal_region=$tmpdir/side-parent-terminal.txt
production_regions=$tmpdir/side-ingress-production.txt
hotside_region=$tmpdir/side-ingress-hotside.txt
recorder_region=$tmpdir/side-ingress-recorder-preflight.txt
ins_region=$tmpdir/side-ingress-ins.txt
first_publish_ingress_region=$tmpdir/side-ingress-first-publish-test.txt
asm_region=$tmpdir/side-parent-asm-consumption.txt
asm_head_region=$tmpdir/side-parent-asm-head.txt
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS'

require_order()
{
  region=$1
  before=$2
  after=$3
  label=$4
  before_line=$(awk -v needle="$before" 'index($0, needle) { print NR; exit }' \
    "$region")
  after_line=$(awk -v needle="$after" 'index($0, needle) { print NR; exit }' \
    "$region")
  if test -z "$before_line" || test -z "$after_line" || \
     test "$before_line" -ge "$after_line"; then
    echo "ARM64 side-parent lifecycle ordering changed: $label" >&2
    exit 1
  fi
}

nth_line()
{
  region=$1
  needle=$2
  occurrence=$3
  awk -v needle="$needle" -v occurrence="$occurrence" '
    index($0, needle) && ++seen == occurrence { print NR; exit }
  ' "$region"
}

require_nth_order()
{
  region=$1
  before=$2
  before_n=$3
  after=$4
  after_n=$5
  label=$6
  before_line=$(nth_line "$region" "$before" "$before_n")
  after_line=$(nth_line "$region" "$after" "$after_n")
  if test -z "$before_line" || test -z "$after_line" || \
     test "$before_line" -ge "$after_line"; then
    echo "ARM64 side-ingress mutation ordering changed: $label" >&2
    exit 1
  fi
}

test -f "$archive" || {
  echo "ARM64 side-ingress contract requires an existing experimental build" >&2
  exit 1
}
test "$(lipo -archs "$archive")" = arm64

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -arch arm64 -mmacosx-version-min="$minver" $xcflags \
  -I"$root/src" -dM -E -include lj_arch.h -x c /dev/null >"$macros"
grep -E '^#define LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED[[:space:]]+1$' \
  "$macros" >/dev/null
grep -E '^#define LJ_ARM64_JIT_FIRST_SIDE_RECORDER_FAIL_CLOSED[[:space:]]+0$' \
  "$macros" >/dev/null
grep -E '^#define LJ_ARM64_JIT_EXIT_TARGET_SLOTS[[:space:]]+1$' \
  "$macros" >/dev/null

for symbol in _lj_trace_arm64_first_side_loop_valid \
  _lj_trace_test_arm64_first_side_loop_valid \
  _lj_trace_arm64_side_parent_clear \
  _lj_trace_arm64_side_parent_capture \
  _lj_trace_arm64_side_parent_revalidate; do
  nm "$archive" | grep " T ${symbol}$" >/dev/null || {
    echo "experimental archive lacks side-ingress symbol: $symbol" >&2
    exit 1
  }
done

# The complete checkpoint must remain observational. It may acquire fields and
# reread generations, but Stage 1 cannot claim/release ownership, enter SMR,
# update the selected count/topology/target, patch bytecode, or publish state.
awk '/^typedef struct TraceArm64FirstSideLoopView/ { copy=1 }
     copy && /^static int trace_arm64_side_parent_asm_owner_base/ { exit }
     copy { print }' \
  "$root/src/lj_trace.c" >"$helper_region"
test -s "$helper_region"
for required in \
  'TRACE_ARM64_INT_SIDE_ADMITTED' \
  'TRACE_EXITTAB_MCODE' \
  'trace_runnable_acq(T, parent)' \
  'trace_nchild_acq(T)' \
  'trace_nextside_acq(T)' \
  'snap_count_acq(&v->snap[v->exitno]) != SNAPCOUNT_DONE' \
  'UINT32_C(0x00ff0000)' \
  'v->nextofs != v->mapofs+v->nent+footer' \
  'int have_side_parent_slot = 0;' \
  'if (slot == 4u)' \
  'return have_side_parent_slot &&' \
  '(uint8_t)pcbase != 0' \
  'v->exittarget_bits != v->fallback_encoding_bits' \
  'trace_arm64_first_side_pointer_bits(v->exittarget_raw)' \
  'trace_exittarget_arm64_encode(J2G(J), v->fallback)' \
  'live1 != expected || shadow1 != v->startins' \
  'continuation2 != continuation1' \
  'J->parent != parent' \
  'J->exitno != exitno' \
  'pc != continuation' \
  'trace_startpc_acq(&J->cur) != continuation' \
  'trace_traceno_acq(&J->cur) == parent' \
  'J->baseslot != (BCReg)(1+LJ_FR2)' \
  'trace_startins_acq(&J->cur) != BCINS_AD(BC_JMP, 0, 0)' \
  'trace_pc_in_proto_range(pc, proto_bc(pt), pt->sizebc)'; do
  grep -F "$required" "$helper_region" >/dev/null || {
    echo "ARM64 side-ingress invariant changed: $required" >&2
    exit 1
  }
done
if grep -E 'lj_jit_token_(try|release)|lj_trace_state_store|snap_count_(cas|rel)|trace_nchild_inc|trace_nextside_rel|trace_exittarget_arm64_rel|bc_publish|lj_gc2_smr_read_(try|enter|leave)|la_(store|cas|add|or|and)[0-9a-z_]*\(' \
     "$helper_region" >/dev/null; then
  echo "read-only ARM64 side-ingress checkpoint gained a mutation" >&2
  exit 1
fi

# The new token-private lifetime certificate is allowed exactly one one-shot
# SMR admission per operation. It must prove ambient/exact ownership before
# dereferencing J->L, double-capture the published body and PAUTH identity, and
# publish only the exact source/destination local certificate. It may not acquire/release the
# JIT token, wait for SMR, mutate parent metadata or set side admission.
awk '/^static int trace_arm64_side_parent_asm_owner_base/ { copy=1 }
     copy && /^#if defined\(LJ_TRACE_TEST_HELPERS\)/ { exit }
     copy { print }' "$root/src/lj_trace.c" >"$cert_region"
test -s "$cert_region"
for required in \
  'tg = J2TG(J);' \
  'actor = lj_thr_actor_current();' \
  'tid = tg ? lj_tg_tid_acq(tg) : 0;' \
  'tg == NULL || tg->gl != g' \
  '!lj_thr_id_is_owner(tid)' \
  'lj_tg_actor_acq(tg) != actor' \
  'jit_token_acq(g) != tid' \
  '!lj_jit_token_held(J)' \
  'lj_trace_state_load(J) != LJ_TRACE_ASM' \
  'L->tg_hint != tg' \
  'lj_jit_token_held_l(L, J)' \
  'lj_tg_tid_acq(tg) != tid' \
  'lj_thr_actor_current() != actor' \
  'trace_arm64_side_parent_asm_owner_exact(' \
  'lj_gc2_smr_read_try(g)' \
  'lj_gc2_smr_read_leave(g)' \
  'trace_arm64_first_side_metadata_view_valid(' \
  'view.tracev != local->tracev || view.parent != local->body' \
  'view.mcode != local->mcode' \
  'view.continuationins != local->continuationins' \
  'gcref_acq(view.tracev->slot[local->child])' \
  'local.tracev = view.tracev;' \
  'local.body = view.parent;' \
  'local.mcode = view.mcode;' \
  'local.continuation = continuation;' \
  'local.continuationins = view.continuationins;' \
  'local.parent = parent;' \
  'local.exitno = exitno;' \
  'local.child = child;' \
  'J->arm64_side_parent = local;'; do
  grep -F "$required" "$cert_region" >/dev/null || {
    echo "ARM64 side-parent certificate invariant changed: $required" >&2
    exit 1
  }
done
if grep -F 'jit_token_acq(g) != actor' "$cert_region" >/dev/null; then
  echo "ARM64 side-parent owner confused logical TG id with actor id" >&2
  exit 1
fi
test "$(grep -Fc 'lj_gc2_smr_read_try(g)' "$cert_region")" = 2
test "$(grep -Fc 'lj_gc2_smr_read_leave(g)' "$cert_region")" = 2
if grep -E 'lj_gc2_smr_read_enter|lj_jit_token_(try|release)|snap_count_(cas|rel)|trace_nchild_inc|trace_nextside_rel|trace_exittarget_arm64_rel|TRACE_ARM64_INT_SIDE_ADMITTED[^;]*[|=]|la_(store|cas|add|or|and)[0-9a-z_]*\(' \
     "$cert_region" >/dev/null; then
  echo "bounded ARM64 side-parent certificate gained mutation or waiting" >&2
  exit 1
fi

cert_schema=$tmpdir/side-parent-cert-schema.txt
sed -n '/^typedef struct LJTraceArm64SideParentCert/,/^} LJTraceArm64SideParentCert;/p' \
  "$root/src/lj_jit.h" >"$cert_schema"
for field in 'TraceVec *tracev;' 'GCtrace *body;' 'MCode *mcode;' \
  'const BCIns *continuation;' 'BCIns continuationins;' \
  'TraceNo parent;' 'ExitNo exitno;' 'TraceNo child;'; do
  grep -Fx "  $field" "$cert_schema" >/dev/null || {
    echo "ARM64 side-parent stored schema changed: $field" >&2
    exit 1
  }
done
field_count=$(sed '1d;$d' "$cert_schema" | tr -cd ';' | wc -c | tr -d ' ')
if test "$field_count" != 8; then
  echo "ARM64 side-parent stored schema no longer has exactly eight fields" >&2
  exit 1
fi
if grep -E 'mcauth|SnapShot|exittab' "$cert_schema" >/dev/null; then
  echo "ARM64 side-parent stored schema gained redundant identity" >&2
  exit 1
fi
for required in \
  'BCIns continuationins;' \
  'v->continuationins = continuation2;' \
  'a->continuationins == b->continuationins' \
  'uintptr_t mcauth_bits;' \
  'trace_mcauth_acq(T)' \
  'ptrauth_nop_cast(ASMFunction, v->mcode), T' \
  'v->mcauth_bits != trace_arm64_first_side_function_bits(expected)'; do
  grep -F "$required" "$helper_region" >/dev/null || {
    echo "ARM64 side-parent PAUTH view changed: $required" >&2
    exit 1
  }
done

# Pin the embedded value's token-private lifetime. Each destructive transition
# must clear while its exact token transaction and selectors are still available;
# generic terminal release asserts that those paths already did so, then zeros
# defensively before IDLE/token handoff. The normal abort clear means an
# MCODELM restart must capture fresh at the assembler callsite pinned below.
sed -n '/^void lj_trace_initstate(global_State \*g)/,/^}/p' \
  "$root/src/lj_trace.c" >"$init_region"
sed -n '/^static void trace_terminal_pin_preflight(global_State \*g)/,/^}/p' \
  "$root/src/lj_trace.c" >"$preflight_region"
sed -n '/^static TraceStartResult trace_start(jit_State \*J)/,/^}/p' \
  "$root/src/lj_trace.c" >"$start_region"
sed -n '/^static TraceStartResult trace_downrec(jit_State \*J)/,/^}/p' \
  "$root/src/lj_trace.c" >"$downrec_region"
sed -n '/^static TraceAbortResult trace_abort(jit_State \*J)/,/^}/p' \
  "$root/src/lj_trace.c" >"$abort_region"
sed -n '/^void lj_trace_abort_owner(lua_State \*L)/,/^}/p' \
  "$root/src/lj_trace.c" >"$abort_owner_region"
sed -n '/^static void trace_terminal_release(lua_State \*L, jit_State \*J)/,/^}/p' \
  "$root/src/lj_trace.c" >"$terminal_region"
for region in "$init_region" "$preflight_region" "$start_region" \
  "$downrec_region" "$abort_region" "$abort_owner_region" \
  "$terminal_region"; do
  test -s "$region"
done
require_order "$init_region" \
  'lj_trace_arm64_side_parent_clear(J);' 'J->trace_reclaim_epoch = 0;' \
  'initialization clear before recorder-state initialization'
require_order "$start_region" \
  'lj_assertJ(lj_jit_token_held(J),' 'lj_trace_arm64_side_parent_clear(J);' \
  'new-trace clear after token-owner assertion'
require_order "$start_region" \
  'lj_trace_arm64_side_parent_clear(J);' 'J->root_startins_pending = 0;' \
  'new-trace clear before parent/root selection'
require_order "$downrec_region" \
  'lj_trace_arm64_side_parent_clear(J);' 'if (bc_op(ins) == BC_RETM)' \
  'down-recursion clear before terminal early return'
require_order "$downrec_region" \
  'lj_trace_arm64_side_parent_clear(J);' 'J->parent = 0;' \
  'down-recursion clear before parent repurpose'
require_order "$abort_region" \
  'lj_trace_arm64_side_parent_clear(J);' 'lj_mcode_abort(J);' \
  'trace abort clear before mcode teardown'
require_order "$abort_owner_region" \
  'if (!lj_jit_token_held_l(L, J))' 'lj_trace_arm64_side_parent_clear(J);' \
  'owner abort authenticates before private clear'
require_order "$abort_owner_region" \
  'lj_trace_arm64_side_parent_clear(J);' 'lj_trace_state_abort(J);' \
  'owner abort clear before scratch teardown'
require_order "$terminal_region" \
  'lj_assertJ(J->arm64_side_parent.body == NULL,' \
  'lj_trace_arm64_side_parent_clear(J);' \
  'terminal assertion before defensive clear'
require_order "$terminal_region" \
  'lj_trace_arm64_side_parent_clear(J);' \
  'lj_tg_vmstate_store_rel(tg, (int32_t)~LJ_VMST_INTERP);' \
  'terminal clear before TG interpreter-state restoration'
require_order "$terminal_region" \
  'lj_tg_vmstate_store_rel(tg, (int32_t)~LJ_VMST_INTERP);' \
  'setvmstate(g, INTERP);' \
  'terminal TG interpreter-state restoration before legacy mirror'
require_order "$terminal_region" \
  'setvmstate(g, INTERP);' \
  'lj_trace_state_store(J, LJ_TRACE_IDLE);' \
  'terminal VM-state restoration before IDLE publication'
require_order "$terminal_region" \
  'lj_trace_state_store(J, LJ_TRACE_IDLE);' 'lj_jit_token_release_l(L, J);' \
  'terminal IDLE publication before token release'
grep -F 'if (J->arm64_side_parent.body != NULL) {' \
  "$preflight_region" >/dev/null || {
  echo "ARM64 side-parent shutdown preflight lost empty-body requirement" >&2
  exit 1
}
require_order "$preflight_region" \
  'if (J->arm64_side_parent.body != NULL) {' 'if (J->curfinal' \
  'shutdown certificate preflight before scratch-body checks'

# The broad side gate remains closed, while the ordinary build opens exactly
# the certified first child. Pin the two production hot-exit checkpoints, the
# token-owned per-instruction preflight, and the separately guarded one-shot
# test seam. Unsupported first sides and side-of-side must still fail closed.
test "$(grep -Fc 'lj_trace_arm64_first_side_loop_valid(' \
  "$root/src/lj_trace.c")" = 7
sed -n '/^static TraceStartResult trace_start(jit_State \*J)/,/^}/p' \
  "$root/src/lj_trace.c" >"$production_regions"
sed -n '/^static int trace_arm64_first_side_recorder_preflight(/,/^}/p' \
  "$root/src/lj_trace.c" >"$recorder_region"
sed -n '/^void lj_trace_ins(jit_State \*J, const BCIns \*pc)/,/^static int trace_hot_root_start_valid/p' \
  "$root/src/lj_trace.c" >"$ins_region"
sed -n '/^static void trace_hotside(jit_State \*J, const BCIns \*pc,/,/^}/p' \
  "$root/src/lj_trace.c" >"$hotside_region"
cat "$hotside_region" >>"$production_regions"
test -s "$ins_region"
test -s "$hotside_region"
test -s "$recorder_region"
cat "$ins_region" >>"$production_regions"
sed -n '/^static int trace_arm64_first_side_publish_test_preflight(/,/^}/p' \
  "$root/src/lj_trace.c" >"$first_publish_ingress_region"
test -s "$first_publish_ingress_region"
test "$(grep -Fc 'trace_arm64_first_side_recorder_preflight(' \
  "$first_publish_ingress_region")" = 1
test "$(grep -Fc 'trace_arm64_first_side_publish_test_preflight(' \
  "$ins_region")" = 1
grep -F 'trace_arm64_first_side_publish_test_active(' \
  "$first_publish_ingress_region" >/dev/null
test "$(grep -Fc 'lj_trace_arm64_first_side_loop_valid(' \
  "$production_regions")" = 2
test "$(grep -Fc 'LJ_TRACE_ARM64_SIDE_CONTEXT_IDLE' \
  "$production_regions")" = 1
test "$(grep -Fc 'LJ_TRACE_ARM64_SIDE_CONTEXT_CLAIM' \
  "$production_regions")" = 1
test "$(grep -Fc 'trace_arm64_first_side_recorder_preflight(J, pc, &L)' \
  "$ins_region")" = 1
test "$(grep -Fc 'lj_trace_arm64_first_side_loop_valid(' \
  "$recorder_region")" = 1
grep -F 'LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER' "$recorder_region" >/dev/null
require_order "$production_regions" \
  'LJ_TRACE_ARM64_SIDE_CONTEXT_IDLE' 'lj_jit_token_try_l(L, J)' \
  'idle certificate before token claim'
require_order "$production_regions" \
  'lj_jit_token_try_l(L, J)' 'LJ_TRACE_ARM64_SIDE_CONTEXT_CLAIM' \
  'token claim before claim certificate'
require_order "$production_regions" \
  'LJ_TRACE_ARM64_SIDE_CONTEXT_CLAIM' 'jit_owner_l_rel(J, L)' \
  'claim certificate before owner publication'
require_nth_order "$hotside_region" \
  'lj_gc2_smr_read_try(g)' 1 \
  'LJ_TRACE_ARM64_SIDE_CONTEXT_IDLE' 1 \
  'SMR admission before idle certificate'
require_nth_order "$hotside_region" \
  'LJ_TRACE_ARM64_SIDE_CONTEXT_IDLE' 1 \
  'snap_count_cas_acqrel(snap, &count, count + 1u)' 1 \
  'idle certificate before first count mutation'
require_nth_order "$hotside_region" \
  'snap_count_cas_acqrel(snap, &count, count + 1u)' 1 \
  'lj_jit_token_try_l(L, J)' 1 \
  'pre-threshold count before token claim'
require_nth_order "$hotside_region" \
  'lj_jit_token_try_l(L, J)' 1 \
  'parentT = traceref_safe(J, parent);' 2 \
  'token claim before protected parent reload'
require_nth_order "$hotside_region" \
  'parentT = traceref_safe(J, parent);' 2 \
  'LJ_TRACE_ARM64_SIDE_CONTEXT_CLAIM' 1 \
  'protected parent reload before claim certificate'
require_nth_order "$hotside_region" \
  'LJ_TRACE_ARM64_SIDE_CONTEXT_CLAIM' 1 \
  'snap_count_cas_acqrel(snap, &count, count + 1u)' 2 \
  'claim certificate before terminal count mutation'
require_nth_order "$hotside_region" \
  'snap_count_cas_acqrel(snap, &count, count + 1u)' 2 \
  'jit_owner_l_rel(J, L)' 1 \
  'terminal count mutation before owner publication'
require_nth_order "$hotside_region" \
  'lj_jit_token_release_l(L, J);' 1 \
  'lj_gc2_smr_read_leave(g);' 2 \
  'post-token request releases token before SMR lease'
require_nth_order "$hotside_region" \
  'lj_jit_token_release_l(L, J);' 4 \
  'lj_gc2_smr_read_leave(g);' 3 \
  'failed claim releases token before request-path SMR leave'
require_nth_order "$hotside_region" \
  'lj_jit_token_release_l(L, J);' 4 \
  'lj_gc2_smr_read_leave(g);' 5 \
  'failed claim releases token before shared out-path SMR leave'

# The assembler is the sole production consumer. Capture occurs exactly once
# after semantic admission and before the first fallible allocation. Three
# revalidations live in lj_asm_trace (pre-regsp, pre-tail and post-snapshot),
# plus the pre-head revalidation before raw parent/map consumption.
sed -n '/^void lj_asm_trace(jit_State \*J, GCtrace \*T)/,/^}/p' \
  "$root/src/lj_asm.c" >"$asm_region"
sed -n '/^static void asm_head_side(ASMState \*as)/,/^}/p' \
  "$root/src/lj_asm.c" >"$asm_head_region"
test -s "$asm_region" && test -s "$asm_head_region"
test "$(grep -F 'lj_trace_arm64_side_parent_capture(' \
  "$root"/src/*.c | wc -l | tr -d ' ')" = 2 || {
  echo "ARM64 side-parent capture is not definition plus sole assembler call" >&2
  exit 1
}
test "$(grep -F 'lj_trace_arm64_side_parent_revalidate(' \
  "$root"/src/*.c | wc -l | tr -d ' ')" = 5 || {
  echo "ARM64 side-parent revalidation call set changed" >&2
  exit 1
}
test "$(grep -Fc 'lj_trace_arm64_side_parent_capture(J)' \
  "$asm_region")" = 1
test "$(grep -Fc 'lj_trace_arm64_side_parent_revalidate(J)' \
  "$asm_region")" = 3
test "$(grep -Fc 'lj_trace_arm64_side_parent_revalidate(as->J)' \
  "$asm_head_region")" = 1
require_order "$asm_region" 'lj_asm_arm64_side_ir_admit(' \
  'lj_trace_arm64_side_parent_capture(J)' \
  'side semantic admission before parent capture'
require_order "$asm_region" 'J->loopref != 0' \
  'lj_asm_arm64_side_ir_admit(' \
  'side non-loop shape before semantic admission'
require_order "$asm_region" 'lj_trace_arm64_side_parent_capture(J)' \
  'as->orignins = lj_ir_nextins(J);' \
  'parent capture before first fallible IR growth'
require_order "$asm_region" 'lj_trace_arm64_side_parent_revalidate(J)' \
  'asm_setup_regsp(as);' \
  'parent revalidation before regsp map construction'
require_order "$asm_head_region" \
  'lj_trace_arm64_side_parent_revalidate(as->J)' \
  'trace_ir_acq(as->parent)' \
  'pre-head revalidation before parent dereference'
grep -F 'result == LJ_TRACE_ARM64_SIDE_PARENT_SMR_RETRY ?' \
  "$root/src/lj_asm.c" >/dev/null
grep -F 'LJ_TRERR_SMRRETRY : LJ_TRERR_RETRY' \
  "$root/src/lj_asm.c" >/dev/null

fixture_source=$root/tests/t-arm64-jit-side-ingress-metadata.c
for required in \
  'SIDE_META_PARENT = 1' \
  'SIDE_META_EXIT = 2' \
  'SIDE_META_PC_POS = 13' \
  'TRACE_ARM64_INT_SIDE_ADMITTED' \
  'TRACE_EXITTAB_MCODE' \
  'trace_exittarget_arm64_rel(G(L), T, (ExitNo)i, fallback);' \
  'trace_exittarget_arm64_encode((global_State *)(void *)T,' \
  'saved_entry | SNAP_NORESTORE' \
  'saved_entry | UINT32_C(0x00800000)' \
  'SNAP(4, 0, SIDE_META_R_VALUE)' \
  'SNAP(2, 0, SIDE_META_R_VALUE)' \
  'SNAP(4, 0, REF_DROP)' \
  'selected->count = SNAPCOUNT_DONE;' \
  'f->snap[SIDE_META_EXIT].count == before' \
  'J->parent = SIDE_META_PARENT+1;' \
  'J->exitno = SIDE_META_EXIT-1;' \
  'J->pc = pc-1;' \
  'side_meta_check_at(L, f, pc, pc+1,' \
  '&proto_bc(f->pt)[f->pt->sizebc]' \
  'setmref(J->cur.startpc, pc+1);' \
  'trace_traceno_rel(&J->cur, SIDE_META_PARENT);' \
  'J->cur.startins = BCINS_AD(BC_JMP, 1, 0);' \
  'J->baseslot++;' \
  'lj_trace_state_store(J, LJ_TRACE_RECORD_1ST);' \
  'LJ_TRACE_ARM64_SIDE_CONTEXT_METADATA' \
  'LJ_TRACE_ARM64_SIDE_CONTEXT_IDLE' \
  'LJ_TRACE_ARM64_SIDE_CONTEXT_CLAIM' \
  'LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER'; do
  grep -F "$required" "$fixture_source" >/dev/null || {
    echo "ARM64 side-ingress mutation coverage changed: $required" >&2
    exit 1
  }
done
for required in \
  'LJ_TRACE_ARM64_SIDE_PARENT_SMR_RETRY == -1' \
  'LJ_TRACE_ARM64_SIDE_PARENT_RETRY == 0' \
  'LJ_TRACE_ARM64_SIDE_PARENT_OK == 1' \
  'test_parent_lifetime_certificate(L, &fixture)' \
  'J->arm64_side_parent.tracev = (TraceVec *)&f->replacement_tracev;' \
  'J->arm64_side_parent.body = &replacement;' \
  'J->arm64_side_parent.mcode = cert.mcode+1;' \
  'J->arm64_side_parent.continuation = continuation+1;' \
  'J->arm64_side_parent.continuationins ^= UINT32_C(0x00000100);' \
  'J->arm64_side_parent.parent++;' \
  'J->arm64_side_parent.exitno--;' \
  'J->arm64_side_parent.child++;' \
  'f->replacement_tracev = f->tracev;' \
  'setgcrefrel(f->tracev.slot[SIDE_META_CHILD], NULL);' \
  '(const GCobj *)LJ_TRACE_PENDING);' \
  'setgcrefrel(f->tracev.slot[SIDE_META_PARENT], obj2gco(&replacement));' \
  'saved_continuationins ^ UINT32_C(0x00000100));' \
  'gc2_smr_reclaiming_cas(' \
  'ptrauth_nop_cast(ASMFunction, T->mcode));' \
  'ptrauth_nop_cast(ASMFunction, T->mcode), &wrong_discriminator' \
  'la_storefunc_rel(&T->mcauth, NULL);'; do
  grep -F "$required" "$fixture_source" >/dev/null || {
    echo "ARM64 side-parent mutation coverage changed: $required" >&2
    exit 1
  }
done
test "$(grep -Fc 'assert(gc2_smr_readers_acq(g) == 0);' \
  "$fixture_source")" -ge 7

# arm64e compilation proves that both the exact PAC encoding check and its
# wrong-discriminator mutation stay well-typed before publication is opened.
# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O0 -Wall -Wextra -Werror -arch arm64e \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  -c "$root/src/lj_trace.c" -o "$audit_object"
# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O0 -Wall -Wextra -Werror -arch arm64e \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  -c "$fixture_source" -o "$fixture_arm64e"

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  "$fixture_source" "$archive" -lm -pthread -o "$fixture"
"$fixture"

echo "arm64_jit_side_ingress_metadata_contract OK: exact first-level LOOP idle/claim/owner admission, parent lifetime/PAUTH and assembler-consumption boundary verified; broad side gate remains closed"
