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
  '(uint8_t)pcbase != 0' \
  'v->exittarget_raw != v->fallback_encoding' \
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
# publish only the six-field local certificate. It may not acquire/release the
# JIT token, wait for SMR, mutate parent metadata or set side admission.
awk '/^static int trace_arm64_side_parent_asm_owner_base/ { copy=1 }
     copy && /^#ifdef LJ_TRACE_TEST_HELPERS/ { exit }
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
  'view.parent == local.body && view.mcode == local.mcode' \
  'view.continuationins == local.continuationins' \
  'local.body = view.parent;' \
  'local.mcode = view.mcode;' \
  'local.continuation = continuation;' \
  'local.continuationins = view.continuationins;' \
  'local.parent = parent;' \
  'local.exitno = exitno;' \
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
for field in 'GCtrace *body;' 'MCode *mcode;' \
  'const BCIns *continuation;' 'BCIns continuationins;' \
  'TraceNo parent;' 'ExitNo exitno;'; do
  grep -Fx "  $field" "$cert_schema" >/dev/null || {
    echo "ARM64 side-parent stored schema changed: $field" >&2
    exit 1
  }
done
field_count=$(sed '1d;$d' "$cert_schema" | tr -cd ';' | wc -c | tr -d ' ')
if test "$field_count" != 6; then
  echo "ARM64 side-parent stored schema no longer has exactly six fields" >&2
  exit 1
fi
if grep -E 'mcauth|TraceVec|tracev' "$cert_schema" >/dev/null; then
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

# Pin the embedded value's dormant lifetime. Each destructive transition must
# clear while its exact token transaction and selectors are still available;
# generic terminal release asserts that those paths already did so, then zeros
# defensively before IDLE/token handoff. The normal abort clear means an
# MCODELM restart must capture fresh; that future callsite is not frozen here.
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
  'lj_trace_state_store(J, LJ_TRACE_IDLE);' \
  'terminal clear before IDLE publication'
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

# Pin the dormant boundary: the only core-name occurrences are its comment,
# definition and test-wrapper call, and none is in a production ingress.
test "$(grep -Fc 'lj_trace_arm64_first_side_loop_valid(' \
  "$root/src/lj_trace.c")" = 3
sed -n '/^static TraceStartResult trace_start(jit_State \*J)/,/^}/p' \
  "$root/src/lj_trace.c" >"$production_regions"
sed -n '/^void lj_trace_ins(jit_State \*J, const BCIns \*pc)/,/^}/p' \
  "$root/src/lj_trace.c" >>"$production_regions"
sed -n '/^static void trace_hotside(jit_State \*J, const BCIns \*pc,/,/^}/p' \
  "$root/src/lj_trace.c" >>"$production_regions"
if grep -F 'lj_trace_arm64_first_side_loop_valid' \
     "$production_regions" >/dev/null; then
  echo "Stage 1 side-ingress checkpoint was wired into production" >&2
  exit 1
fi
for dormant in lj_trace_arm64_side_parent_capture \
  lj_trace_arm64_side_parent_revalidate; do
  test "$(grep -F "$dormant(" "$root"/src/*.c | wc -l | tr -d ' ')" = 1 || {
    echo "dormant ARM64 side-parent API gained a production call: $dormant" >&2
    exit 1
  }
done

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
  'J->arm64_side_parent.body = &replacement;' \
  'J->arm64_side_parent.mcode = cert.mcode+1;' \
  'J->arm64_side_parent.continuation = continuation+1;' \
  'J->arm64_side_parent.continuationins ^= UINT32_C(0x00000100);' \
  'J->arm64_side_parent.parent++;' \
  'J->arm64_side_parent.exitno--;' \
  'f->replacement_tracev = f->tracev;' \
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

echo "arm64_jit_side_ingress_metadata_contract OK: dormant first-level LOOP parent/snapshot/owner and lifetime/PAUTH certificates verified; side recorder remains closed"
