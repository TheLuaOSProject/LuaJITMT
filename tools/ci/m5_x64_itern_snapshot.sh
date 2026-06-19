#!/bin/sh
# Guard x64 BC_ITERN array/hash slot snapshots.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
OUT=${TMPDIR:-/tmp}/lj_t-x64-itern-forward

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff -e '
local t = { [0] = "z", "a", nil, "c", alpha = 11, beta = 12 }
local seen, n = {}, 0
for k, v in pairs(t) do
  seen[k] = v
  n = n + 1
end
assert(n == 5)
assert(seen[0] == "z" and seen[1] == "a" and seen[3] == "c")
assert(seen.alpha == 11 and seen.beta == 12)
'

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-x64-itern-forward.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
"$OUT"

for needle in \
  'lea r8, [RB+TAB_COLO_SLOTS]' \
  'mov TMPRd, dword [ITYPE+TABARRAY_ASIZE_OFS]' \
  'mov r8, [ITYPE+RC*8]' \
  'mov64 r9, LJ_TFORWARD_BITS' \
  'call extern lj_tab_itern_forward' \
  'cmp r8, LJ_TNIL; je >4' \
  'mov [BASE+RA*8+8], r8' \
  'mov r8, TAB:RB->node' \
  'mov r9d, dword [r8+TABNODE_HMASK_OFS]' \
  'mov r8, NODE:ITYPE->val' \
  'cmp r8, LJ_TNIL; je >7' \
  'mov r9, NODE:ITYPE->key' \
  'mov [BASE+RA*8+8], r8' \
  't-x64-itern-forward OK'
do
  if ! rg -F -q "$needle" "$ROOT/src/vm_x64.dasc" \
      "$ROOT/tests/t-x64-itern-forward.c"; then
    echo "guardrail: missing x64 BC_ITERN snapshot marker: $needle" >&2
    exit 1
  fi
done

if ! awk '
  /case BC_ITERN:/ { initern = 1 }
  initern && /mov64 r9, LJ_TFORWARD_BITS/ { forward++ }
  initern && /call extern lj_tab_itern_forward/ { helper++ }
  initern && /case BC_ISNEXT:/ { initern = 0 }
  END { exit forward >= 2 && helper >= 2 ? 0 : 1 }
' "$ROOT/src/vm_x64.dasc"; then
  echo "guardrail: x64 BC_ITERN must resolve forwarded array/hash slots in C" >&2
  exit 1
fi

for reject in \
  'cmp aword [ITYPE+RC*8], LJ_TNIL' \
  'cmp aword NODE:ITYPE->val, LJ_TNIL' \
  'mov RB, [ITYPE+RC*8]' \
  'mov RC, NODE:ITYPE->val' \
  'cmp RCd, TAB:RB->hmask' \
  'add NODE:ITYPE, TAB:RB->node'
do
  if rg -F -n "$reject" "$ROOT/src/vm_x64.dasc"; then
    echo "guardrail: x64 BC_ITERN must use header snapshots and single slot reads: $reject" >&2
    exit 1
  fi
done

echo "M5 x64 BC_ITERN node-header snapshot guard passed"
