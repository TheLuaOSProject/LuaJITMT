#!/bin/sh
# Guard x64 TGET* array fast paths against stale separated-array bounds.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff -e '
local t = {}
for i = 1, 96 do t[i] = i * 3 end
assert(t[64] == 192)
local k = 70
assert(t[k] == 210)
local function getv(a, key) return a[key] end
assert(getv(t, 80) == 240)
assert(t[120] == nil)
'

for needle in \
  'TAB_COLO_SLOTS' \
  'TABARRAY_ASIZE_OFS' \
  'mov TMPR, TAB:RB->array' \
  'lea r8, [RB+TAB_COLO_SLOTS]' \
  'mov ITYPEd, dword [TMPR+TABARRAY_ASIZE_OFS]' \
  'cmp RCd, ITYPEd' \
  'add RC, TMPR'
do
  if ! rg -F -q "$needle" "$ROOT/src/vm_x64.dasc"; then
    echo "guardrail: missing x64 TGET array-header marker: $needle" >&2
    exit 1
  fi
done

if ! awk '
  /case BC_TGET[VBR]:/ { in_get = 1; checked++; next }
  in_get && /mov TMPR, TAB:RB->array/ { array++ }
  in_get && /lea r8, \[RB\+TAB_COLO_SLOTS\]/ { colo++ }
  in_get && /mov ITYPEd, dword \[TMPR\+TABARRAY_ASIZE_OFS\]/ { hdr++ }
  in_get && /cmp RCd, ITYPEd/ { cmp++ }
  in_get && /add RC, TMPR/ { add++ }
  in_get && /cmp RCd, TAB:RB->asize/ { bad = 1 }
  in_get && /add RC, TAB:RB->array/ { bad = 1 }
  in_get && /break;/ { in_get = 0 }
  END {
    exit checked == 3 && array == 3 && colo == 3 && hdr == 3 &&
	 cmp == 3 && add == 3 && !bad ? 0 : 1
  }
' "$ROOT/src/vm_x64.dasc"; then
  echo "guardrail: x64 TGETV/TGETB/TGETR must use array-header bounds" >&2
  exit 1
fi

echo "M5 x64 TGET array-header guard passed"
