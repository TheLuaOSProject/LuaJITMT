#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_side_ingress_metadata_contract SKIP: requires native macOS arm64"
  exit 0
fi

archive=$root/src/libluajit.a
asm_source=$root/src/lj_asm.c
trace_source=$root/src/lj_trace.c
if test ! -f "$archive"; then
  echo "ARM64 side-ingress contract requires an experimental archive" >&2
  exit 1
fi
if test "$(lipo -archs "$archive")" != arm64; then
  echo "ARM64 side-ingress archive must contain exactly arm64" >&2
  exit 1
fi

# Entry into side recording is a read-only, exact-token operation. The selected
# parent view is captured first, then the child inherits only a copied snapshot
# and independently validates its scalar semantic IR. The assembler rechecks
# that selection before constructing allocator state and before consuming it.
require_order()
{
  file=$1
  before=$2
  after=$3
  label=$4
  before_line=$(awk -v needle="$before" 'index($0, needle) { print NR; exit }' "$file")
  after_line=$(awk -v needle="$after" 'index($0, needle) { print NR; exit }' "$file")
  if test -z "$before_line" || test -z "$after_line" ||
     test "$before_line" -ge "$after_line"; then
    echo "ARM64 side-ingress ordering changed: $label" >&2
    exit 1
  fi
}

asm_region=$(mktemp "${TMPDIR:-/tmp}/lj-side-asm.XXXXXX")
head_region=$(mktemp "${TMPDIR:-/tmp}/lj-side-head.XXXXXX")
trap 'rm -f "$asm_region" "$head_region"' EXIT HUP INT TERM
sed -n '/^void lj_asm_trace(jit_State \*J, GCtrace \*T)/,/^}/p' \
  "$asm_source" >"$asm_region"
sed -n '/^static void asm_head_side(ASMState \*as)/,/^}/p' \
  "$asm_source" >"$head_region"
test -s "$asm_region" && test -s "$head_region"

grep -F 'lj_trace_arm64_side_parent_capture(J)' "$asm_region" >/dev/null
grep -F 'lj_trace_arm64_side_parent_snapshot(' "$asm_region" >/dev/null
grep -F 'lj_asm_arm64_side_scalar_ir_admit(' "$asm_region" >/dev/null
grep -F 'lj_trace_arm64_side_parent_revalidate(J)' "$asm_region" >/dev/null
grep -F 'lj_trace_arm64_side_parent_revalidate(as->J)' "$head_region" >/dev/null
require_order "$asm_region" 'lj_trace_arm64_side_parent_capture(J)' \
  'lj_trace_arm64_side_parent_snapshot(' 'capture before selected snapshot'
require_order "$asm_region" 'lj_trace_arm64_side_parent_snapshot(' \
  'lj_asm_arm64_side_scalar_ir_admit(' 'snapshot before semantic walk'
require_order "$asm_region" 'lj_trace_arm64_side_parent_revalidate(J)' \
  'asm_setup_regsp(as);' 'selected parent still valid before allocator map'
require_order "$head_region" \
  'lj_trace_arm64_side_parent_revalidate(as->J)' \
  'trace_ir_acq(as->parent)' 'selected parent valid before native header read'

# The exact selected-snapshot and parent body are copied under one nonblocking
# SMR read scope. Refusal releases that scope; it never waits for a peer.
parent_capture=$(sed -n '/^LJTraceArm64SideParentResult$/,/^}/p' "$trace_source")
printf '%s\n' "$parent_capture" | grep -F 'lj_gc2_smr_read_try' >/dev/null
printf '%s\n' "$parent_capture" | grep -F 'lj_gc2_smr_read_leave' >/dev/null
printf '%s\n' "$parent_capture" | grep -F 'LJ_TRACE_ARM64_SIDE_PARENT_RETRY' >/dev/null
if printf '%s\n' "$parent_capture" | grep -E 'lj_gc2_smr_read_(enter|wait)|pthread_mutex|pthread_cond_wait' >/dev/null; then
  echo "ARM64 side-ingress capture gained a blocking lifetime wait" >&2
  exit 1
fi

# The executable admission contract checks actual native parent-to-child
# dispatch and result adaptation; publication and rollback phases are exercised
# by the dedicated first-side transaction contract.
LJ_TEST_ROOT="$root" sh "$root/tools/ci/arm64_jit_side_ir_admission_contract.sh"

echo "arm64_jit_side_ingress_metadata_contract OK: selected snapshot ownership and revalidation verified"
