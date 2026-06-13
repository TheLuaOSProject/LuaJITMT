#!/bin/sh
# Guard x64 TSET* previous-value nil-decision snapshots.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff -e '
local mt = {
  __newindex = function(t, k, v) rawset(t, "hit", tostring(k) .. ":" .. tostring(v)) end
}
local t = setmetatable({ a = 1 }, mt)
t.a = 2
assert(t.a == 2 and t.hit == nil)
t.b = 3
assert(t.hit == "b:3")
local a = { 1, 2 }
a[1] = 10
local k = 2
a[k] = 20
assert(a[1] == 10 and a[2] == 20)
'

for needle in \
  'mov r8, [RC]' \
  'mov r8, [TMPR]' \
  'cmp r8, LJ_TNIL'
do
  if ! rg -F -q "$needle" "$ROOT/src/vm_x64.dasc"; then
    echo "guardrail: missing x64 TSET nil snapshot marker: $needle" >&2
    exit 1
  fi
done

for reject in \
  'cmp aword [RC], LJ_TNIL' \
  'cmp aword [TMPR], LJ_TNIL'
do
  if rg -F -n "$reject" "$ROOT/src/vm_x64.dasc"; then
    echo "guardrail: x64 TSET nil decisions must load slot snapshots first: $reject" >&2
    exit 1
  fi
done

echo "M5 x64 TSET nil snapshot guard passed"
