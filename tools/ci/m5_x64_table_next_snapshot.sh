#!/bin/sh
# Guard x64 lj_vm_next hash-slot nil-decision snapshots.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

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

for needle in \
  'mov NEXT_TMP, NODE:NEXT_PTR->val' \
  'cmp NEXT_TMP, LJ_TNIL; je >7'
do
  if ! rg -F -q "$needle" "$ROOT/src/vm_x64.dasc"; then
    echo "guardrail: missing x64 table next snapshot marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'cmp qword NODE:NEXT_PTR->val, LJ_TNIL' \
    "$ROOT/src/vm_x64.dasc"; then
  echo "guardrail: x64 lj_vm_next must load hash slot values before nil decisions" >&2
  exit 1
fi

echo "M5 x64 table next snapshot guard passed"
