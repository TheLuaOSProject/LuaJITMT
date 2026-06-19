#!/bin/sh
# Guard x64 lj_vm_next hash-slot nil-decision snapshots.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
OUT=${TMPDIR:-/tmp}/lj_t-x64-vm-next-forward

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -e '
local t = { [0] = "z", "a", nil, "c", x = 41, y = 42 }
local seen = {}
for k, v in next, t, nil do seen[k] = v end
assert(seen[0] == "z" and seen[1] == "a" and seen[3] == "c")
assert(seen.x == 41 and seen.y == 42)
local n = 0
for k, v in pairs(t) do
  assert(seen[k] == v)
  n = n + 1
end
assert(n == 5)
'

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-x64-vm-next-forward.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
"$OUT"

for needle in \
  'mov r10, NEXT_TAB->array' \
  'lea NEXT_TMP, [NEXT_TAB+TAB_COLO_SLOTS]' \
  'mov NEXT_ASIZE, dword [r10+TABARRAY_ASIZE_OFS]' \
  'mov NEXT_TMP, qword [r10+NEXT_IDX*8]' \
  'mov64 r11, LJ_TFORWARD_BITS' \
  'call extern lj_tab_vmnext_forward' \
  'mov r8, NEXT_TAB->node' \
  'mov r9d, dword [r8+TABNODE_HMASK_OFS]' \
  'mov NEXT_TMP, NODE:NEXT_PTR->val' \
  'cmp NEXT_TMP, LJ_TNIL; je >7' \
  't-x64-vm-next-forward OK'
do
  if ! rg -F -q "$needle" "$ROOT/src/vm_x64.dasc" \
      "$ROOT/src/lj_tab.c" "$ROOT/tests/t-x64-vm-next-forward.c"; then
    echo "guardrail: missing x64 table next snapshot marker: $needle" >&2
    exit 1
  fi
done

if ! awk '
  /->vm_next:/ { innext = 1 }
  innext && /mov64 r11, LJ_TFORWARD_BITS/ { forward++ }
  innext && /call extern lj_tab_vmnext_forward/ { helper++ }
  innext && /->vm_next_1:/ { innext = 0 }
  END { exit forward >= 2 && helper >= 2 ? 0 : 1 }
' "$ROOT/src/vm_x64.dasc"; then
  echo "guardrail: x64 lj_vm_next must resolve forwarded array/hash slots in C" >&2
  exit 1
fi

if rg -n 'cmp qword NODE:NEXT_PTR->val, LJ_TNIL' \
    "$ROOT/src/vm_x64.dasc"; then
  echo "guardrail: x64 lj_vm_next must load hash slot values before nil decisions" >&2
  exit 1
fi

for reject in \
  'cmp NEXT_IDX, NEXT_TAB->hmask' \
  'add NODE:NEXT_PTR, NEXT_TAB->node' \
  'mov NEXT_TMP, NEXT_TAB->array'
do
  if rg -F -n "$reject" "$ROOT/src/vm_x64.dasc"; then
    echo "guardrail: x64 lj_vm_next must use one array/header and node/header pair: $reject" >&2
    exit 1
  fi
done

echo "M5 x64 table next node-header snapshot guard passed"
