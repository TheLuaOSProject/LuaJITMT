#!/bin/sh
# Guard x64 ipairs_aux array-slot snapshots.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
OUT=${TMPDIR:-/tmp}/lj_t-x64-ipairs-forward

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff -e '
local t = { 10, 20, nil, 40 }
local n, sum = 0, 0
for i, v in ipairs(t) do
  n = n + 1
  sum = sum + i + v
end
assert(n == 2 and sum == 33)
'

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-x64-ipairs-forward.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
"$OUT"

for needle in \
  'TABARRAY_ASIZE_OFS' \
  'lea r8, [RB+TAB_COLO_SLOTS]' \
  'mov TMPRd, dword [RD+TABARRAY_ASIZE_OFS]' \
  'mov r8, [RD]' \
  'cmp r8, LJ_TNIL;  je ->fff_res0' \
  'mov64 r9, LJ_TFORWARD_BITS' \
  'cmp r8, r9; jne >4' \
  'call extern lj_tab_getint_hop' \
  't-x64-ipairs-forward OK' \
  'mov [BASE-8], r8' \
  'mov r8, TAB:RB->node' \
  'cmp dword [r8+TABNODE_HMASK_OFS], 0; je ->fff_res0'
do
  if ! rg -F -q "$needle" "$ROOT/src/vm_x64.dasc" \
      "$ROOT/src/lj_tab.c" "$ROOT/tests/t-x64-ipairs-forward.c"; then
    echo "guardrail: missing x64 ipairs_aux snapshot marker: $needle" >&2
    exit 1
  fi
done

if ! awk '
  /[|][.]ffunc_2 ipairs_aux/ { infn = 1 }
  infn && /cmp aword \[RD\], LJ_TNIL/ { bad = 1 }
  infn && /mov RB, \[RD\]/ { bad = 1 }
  infn && /cmp dword TAB:RB->hmask, 0/ { bad = 1 }
  infn && /cmp RAd, TAB:RB->asize/ { bad = 1 }
  infn && /mov64 r9, LJ_TFORWARD_BITS/ { forward = 1 }
  infn && /cmp r8, r9/ { forward_cmp = 1 }
  infn && /call extern lj_tab_getint_hop/ { helper = 1 }
  infn && /->fff_res2:/ { exit bad ? 1 : 0 }
  END { if (bad || !forward || !forward_cmp || !helper) exit 1 }
' "$ROOT/src/vm_x64.dasc"; then
  echo "guardrail: x64 ipairs_aux must snapshot slots and resolve FORWARD values" >&2
  exit 1
fi

echo "M5 x64 ipairs_aux node-header snapshot guard passed"
