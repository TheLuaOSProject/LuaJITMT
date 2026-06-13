#!/bin/sh
# Guard x64 ipairs_aux array-slot snapshots.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

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

for needle in \
  'mov r8, [RD]' \
  'cmp r8, LJ_TNIL;  je ->fff_res0' \
  'mov [BASE-8], r8' \
  'mov r8, TAB:RB->node' \
  'cmp dword [r8-8], 0; je ->fff_res0'
do
  if ! rg -F -q "$needle" "$ROOT/src/vm_x64.dasc"; then
    echo "guardrail: missing x64 ipairs_aux snapshot marker: $needle" >&2
    exit 1
  fi
done

if ! awk '
  /[|][.]ffunc_2 ipairs_aux/ { infn = 1 }
  infn && /cmp aword \[RD\], LJ_TNIL/ { bad = 1 }
  infn && /mov RB, \[RD\]/ { bad = 1 }
  infn && /cmp dword TAB:RB->hmask, 0/ { bad = 1 }
  infn && /->fff_res2:/ { exit bad ? 1 : 0 }
  END { if (bad) exit 1 }
' "$ROOT/src/vm_x64.dasc"; then
  echo "guardrail: x64 ipairs_aux must snapshot array slots and check node-header hmask" >&2
  exit 1
fi

echo "M5 x64 ipairs_aux node-header snapshot guard passed"
