#!/bin/sh
# Guard x64 TSET* previous-value nil-decision snapshots.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
OUT=${TMPDIR:-/tmp}/lj_t-x64-tset-forward

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
local s = { spread(96) }
s[64] = 640
local kk = 70
s[kk] = 700
for i = 80, 82 do s[i] = i * 10 end
assert(s[64] == 640 and s[70] == 700 and s[82] == 820)
'

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-x64-tset-forward.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
"$OUT"

for needle in \
  'TABARRAY_ASIZE_OFS' \
  'mov r8, [RC]' \
  'cmp r8, LJ_TNIL' \
  'mov64 r9, LJ_TFORWARD_BITS' \
  'je ->vmeta_tsetv' \
  'je ->vmeta_tsetb' \
  'mov r9d, RCd' \
  'mov64 r10, LJ_TFORWARD_BITS' \
  'jmp ->vmeta_tsetr' \
  'mov ITYPEd, dword [TMPR+TABARRAY_ASIZE_OFS]' \
  'cmp RCd, ITYPEd' \
  'add RC, TMPR' \
  'mov ITYPE, TAB:RB->array' \
  'mov r8d, dword [ITYPE+TABARRAY_ASIZE_OFS]' \
  'cmp RDd, r8d' \
  'add TMPR, ITYPE' \
  'jmp ->vmeta_tsets		// M5: no legacy x64 hash-slot store.' \
  'call extern lj_meta_tsettv_pair' \
  'call extern lj_tab_storetv' \
  'call extern lj_tab_storetvn' \
  'call extern lj_gc_barrierback_tab_g' \
  '->BC_TSETV_RETRY:' \
  '->BC_TSETB_RETRY:' \
  '->BC_TSETR_RETRY:' \
  '->BC_TSETM_RETRY:' \
  'jmp ->BC_TSETV_RETRY' \
  'jmp ->BC_TSETB_RETRY' \
  'jmp ->BC_TSETR_RETRY' \
  'jmp ->BC_TSETM_RETRY' \
  'jmp ->vm_gc2_barriertv_tab' \
  'jmp ->vm_gc2_barriertvn' \
  'jmp ->vm_gc2_barriertab' \
  'call extern lj_gc2_barrier_tv_pair_g' \
  'call extern lj_gc2_barrier_tvn_pair_g' \
  't-x64-tset-forward OK'
do
  if ! rg -F -q -- "$needle" "$ROOT/src/vm_x64.dasc" \
      "$ROOT/tests/t-x64-tset-forward.c"; then
    echo "guardrail: missing x64 TSET nil snapshot marker: $needle" >&2
    exit 1
  fi
done

for reject in \
  'cmp aword [RC], LJ_TNIL' \
  'cmp aword [TMPR], LJ_TNIL' \
  'call extern lj_meta_tset		// (lua_State *L, TValue *o, TValue *k)' \
  'call extern lj_gc2_barrier_tv_g' \
  '.macro barrierback' \
  'barrierback TAB:RB' \
  'mov [RC], RB' \
  'mov [RC], ITYPE'
do
  if rg -F -n -- "$reject" "$ROOT/src/vm_x64.dasc"; then
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
  function start() {
    in_set = 1
    checked++
    array = hdr = cmp = add = 0
  }
  function finish() {
    if (!in_set) return
    if (!array || !hdr || !cmp || !add)
      bad = 1
    in_set = 0
  }
  /case BC_TSETV:/ { finish(); start(); next }
  /case BC_TSETB:/ { finish(); start(); next }
  /case BC_TSETR:/ { finish(); start(); next }
  in_set && /mov TMPR, TAB:RB->array/ { array = 1 }
  in_set && /mov ITYPEd, dword \[TMPR\+TABARRAY_ASIZE_OFS\]/ { hdr = 1 }
  in_set && /cmp RCd, ITYPEd/ { cmp = 1 }
  in_set && /add RC, TMPR/ { add = 1 }
  in_set && /LJ_TFORWARD_BITS/ { forward++ }
  in_set && /->vmeta_tset[vbr]/ { slow++ }
  in_set && /cmp RCd, TAB:RB->asize/ { bad = 1 }
  in_set && /add RC, TAB:RB->array/ { bad = 1 }
  in_set && /break;/ { finish() }
  END {
    finish()
    exit checked == 3 && forward >= 3 && slow >= 3 && !bad ? 0 : 1
  }
' "$ROOT/src/vm_x64.dasc"; then
  :
else
  echo "guardrail: x64 TSET array fast paths must bound slots and slow-path FORWARD values" >&2
  exit 1
fi

if awk '
  /case BC_TSETM:/ {
    in_setm = 1
    saw_storetvn = saw_tvn = 0
    array = hdr = cmp = add = 0
    next
  }
  in_setm && /mov ITYPE, TAB:RB->array/ { array = 1 }
  in_setm && /mov r8d, dword \[ITYPE\+TABARRAY_ASIZE_OFS\]/ { hdr = 1 }
  in_setm && /cmp RDd, r8d/ { cmp = 1 }
  in_setm && /add TMPR, ITYPE/ { add = 1 }
  in_setm && /cmp RDd, TAB:RB->asize/ { bad = 1 }
  in_setm && /add TMPR, TAB:RB->array/ { bad = 1 }
  in_setm && /call extern lj_tab_storetvn/ { saw_storetvn = 1 }
  in_setm && /jmp ->vm_gc2_barriertvn/ { saw_tvn = 1 }
  in_setm && /jmp ->vm_gc2_barriertab/ { bad = 1 }
  in_setm && /mov \[TMPR\], ITYPE/ { bad = 1 }
  in_setm && /break;/ {
    checked = 1
    if (!array || !hdr || !cmp || !add || !saw_storetvn || !saw_tvn)
      bad = 1
    in_setm = 0
  }
  END { exit checked && !bad ? 0 : 1 }
' "$ROOT/src/vm_x64.dasc"; then
  :
else
  echo "guardrail: x64 TSETM must header-bound, publish, range-barrier, and table-barrier batch array slots" >&2
  exit 1
fi

storetv_count=$(awk '
  /call extern lj_tab_storetv[[:space:]]*\/\/ \(lua_State \*L, TValue \*d, TValue \*s\)/ { n++ }
  END { print n + 0 }
' "$ROOT/src/vm_x64.dasc")
if [ "$storetv_count" -ne 3 ]; then
  echo "guardrail: x64 TSET fast paths must publish via lj_tab_storetv" >&2
  exit 1
fi

barrierback_call_count=$(awk '
  /call extern lj_gc_barrierback_tab_g[[:space:]]*\/\/ \(global_State \*g, GCtab \*t\)/ { n++ }
  END { print n + 0 }
' "$ROOT/src/vm_x64.dasc")
if [ "$barrierback_call_count" -ne 4 ]; then
  echo "guardrail: x64 TSET black-table repairs must call lj_gc_barrierback_tab_g for V/B/R/M" >&2
  exit 1
fi

echo "M5 x64 TSET nil snapshot guard passed"
