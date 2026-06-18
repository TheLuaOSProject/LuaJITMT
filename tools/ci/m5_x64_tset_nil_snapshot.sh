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
local function many() return 1, 2, 3 end
local m = { many() }
assert(m[1] == 1 and m[2] == 2 and m[3] == 3)
local function spread(n)
  local r = {}
  for i = 1, n do r[i] = i end
  return unpack(r, 1, n)
end
local big = { spread(96) }
assert(#big == 96 and big[1] == 1 and big[96] == 96)
'

for needle in \
  'mov r8, [RC]' \
  'cmp r8, LJ_TNIL' \
  'jmp ->vmeta_tsets		// M5: no legacy x64 hash-slot store.' \
  'call extern lj_tab_storetv' \
  'call extern lj_tab_storetvn' \
  'jmp ->vm_gc2_barriertv' \
  'jmp ->vm_gc2_barriertvn' \
  'jmp ->vm_gc2_barriertab' \
  'call extern lj_gc2_barrier_tvn_pair_g'
do
  if ! rg -F -q "$needle" "$ROOT/src/vm_x64.dasc"; then
    echo "guardrail: missing x64 TSET nil snapshot marker: $needle" >&2
    exit 1
  fi
done

for reject in \
  'cmp aword [RC], LJ_TNIL' \
  'cmp aword [TMPR], LJ_TNIL' \
  'mov [RC], RB' \
  'mov [RC], ITYPE'
do
  if rg -F -n "$reject" "$ROOT/src/vm_x64.dasc"; then
    echo "guardrail: x64 TSET nil decisions must load slot snapshots first: $reject" >&2
    exit 1
  fi
done

if awk '
  /[|]->BC_TSETS_Z:/ { in_set = 1; next }
  in_set && /jmp ->vmeta_tsets/ { in_set = 0 }
  in_set && /mov \[TMPR\], ITYPE/ { bad = 1 }
  END { exit bad ? 1 : 0 }
' "$ROOT/src/vm_x64.dasc"; then
  :
else
  echo "guardrail: x64 TSETS must not write hash slots directly" >&2
  exit 1
fi

if awk '
  /case BC_TSETM:/ { in_setm = 1; saw_storetvn = 0; saw_tvn = 0; next }
  in_setm && /call extern lj_tab_storetvn/ { saw_storetvn = 1 }
  in_setm && /jmp ->vm_gc2_barriertvn/ { saw_tvn = 1 }
  in_setm && /jmp ->vm_gc2_barriertab/ { bad = 1 }
  in_setm && /mov \[TMPR\], ITYPE/ { bad = 1 }
  in_setm && /break;/ {
    checked = 1
    if (!saw_storetvn || !saw_tvn)
      bad = 1
    in_setm = 0
  }
  END { exit checked && !bad ? 0 : 1 }
' "$ROOT/src/vm_x64.dasc"; then
  :
else
  echo "guardrail: x64 TSETM must publish, range-barrier, and table-barrier batch array slots" >&2
  exit 1
fi

storetv_count=$(rg -F 'call extern lj_tab_storetv' "$ROOT/src/vm_x64.dasc" | wc -l | tr -d ' ')
if [ "$storetv_count" -lt 4 ]; then
  echo "guardrail: x64 TSET fast paths must publish via lj_tab_storetv" >&2
  exit 1
fi

echo "M5 x64 TSET nil snapshot guard passed"
