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
production_regions=$tmpdir/side-ingress-production.txt
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS'

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
  _lj_trace_test_arm64_first_side_loop_valid; do
  nm "$archive" | grep " T ${symbol}$" >/dev/null || {
    echo "experimental archive lacks side-ingress symbol: $symbol" >&2
    exit 1
  }
done

# The complete checkpoint must remain observational. It may acquire fields and
# reread generations, but Stage 1 cannot claim/release ownership, enter SMR,
# update the selected count/topology/target, patch bytecode, or publish state.
awk '/^typedef struct TraceArm64FirstSideLoopView/ { copy=1 }
     copy { print }
     copy && /^\/\* Validate and publish one root-trace entry intent/ { exit }' \
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
  'continuation2 == continuation1' \
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

echo "arm64_jit_side_ingress_metadata_contract OK: dormant first-level LOOP parent/snapshot/owner generations verified; side recorder remains closed"
