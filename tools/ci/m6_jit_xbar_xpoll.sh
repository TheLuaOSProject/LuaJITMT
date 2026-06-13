#!/bin/sh
# Guard existing FFI XBAR aliasing respects XPOLL poll regions.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

make -C "$ROOT/src" >/dev/null

for needle in \
  'static LJ_AINLINE IRRef poll_alias_limit(jit_State *J, IRRef lim)' \
  'J->chain[IR_XBAR] > lim' \
  'J->chain[IR_XPOLL] > lim' \
  'lim = poll_alias_limit(J, lim);' \
  'case IR_NOP: case IR_XBAR:' \
  'LJFOLD(XBAR)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_opt_mem.c" \
      "$ROOT/src/lj_asm.c" "$ROOT/src/lj_opt_fold.c"; then
    echo "guardrail: missing XBAR/XPOLL alias marker: $needle" >&2
    exit 1
  fi
done

uses=$(rg -F 'lim = poll_alias_limit(J, lim);' "$ROOT/src/lj_opt_mem.c" | wc -l)
uses=${uses##* }
if [ "$uses" -lt 2 ]; then
  echo "guardrail: XLOAD forwarding and XSTORE DSE must both honor XPOLL" >&2
  exit 1
fi

if ! rg -F -q 'm6_jit_xbar_xpoll.sh' "$ROOT/tools/ci/m6_jit.sh"; then
  echo "guardrail: m6_jit_xbar_xpoll.sh is not wired into the M6 aggregate" >&2
  exit 1
fi

"$ROOT/tools/ci/m5_jit_hash_store_nyi.sh"

echo "M6 JIT XBAR/XPOLL alias guard passed"
