#!/bin/sh
# Build and guard the x64 local-cell bytecode substrate.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
LUA_PATH_GUARD="$ROOT/src/?.lua;;"
OUT=${TMPDIR:-/tmp}/lj_m5_cell_ops_bc.$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

if ! awk '
  /_\(FUNCCW,/ { funccw = NR }
  /_\(CNEW,/ { cnew = NR }
  /_\(CGET,/ { cget = NR }
  /_\(CSET,/ { cset = NR }
  END { exit(funccw && funccw < cnew && cnew < cget && cget < cset ? 0 : 1) }
' "$ROOT/src/lj_bc.h"; then
  echo "guardrail: CNEW/CGET/CSET must remain after FUNCCW in BCDEF" >&2
  exit 1
fi

for needle in \
  'case BC_CNEW:' \
  'call extern lj_func_newuvcell' \
  'case BC_CGET:' \
  'case BC_CSET:'
do
  if ! rg -F -q "$needle" "$ROOT/src/vm_x64.dasc"; then
    echo "guardrail: missing x64 local-cell VM marker: $needle" >&2
    exit 1
  fi
done

if ! awk '
  /case BC_CSET:/ { incset = 1 }
  incset && /call extern lj_gc_pubuv/ { pubuv = 1 }
  incset && /case BC_USETV:/ { exit(pubuv ? 0 : 1) }
  END { if (!pubuv) exit 1 }
' "$ROOT/src/vm_x64.dasc"; then
  echo "guardrail: BC_CSET must publish closed-cell stores via lj_gc_pubuv" >&2
  exit 1
fi

for needle in \
  'lj_func_newuvcell' \
  'func_celluv' \
  'proto_celluv(pt)' \
  'itype(slot) == LJ_TUPVAL' \
  'gco2uv(gcV(slot))'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_func.c" "$ROOT/src/lj_func.h"; then
    echo "guardrail: missing local-cell function marker: $needle" >&2
    exit 1
  fi
done

for needle in \
  'case BC_CGET:' \
  'debug_localcell' \
  'lj_gc_pubuv(G(L), o)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_debug.c"; then
    echo "guardrail: missing debug local-cell marker: $needle" >&2
    exit 1
  fi
done

for needle in \
  'BC_CNEW' \
  'BC_CGET' \
  'BC_CSET' \
  'PROTO_NOJIT'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_parse.c"; then
    echo "guardrail: missing parser local-cell marker: $needle" >&2
    exit 1
  fi
done

LUA_PATH=$LUA_PATH_GUARD "$ROOT/src/luajit" -bl -e '
local x = 0
local function f()
  x = x + 1
  return x
end
x = 7
local function g()
  local y = 1
  return function()
    y = y + 1
    return y
  end
end
return f, g, x
' >"$OUT"

if ! rg -q 'CGET|CSET' "$OUT"; then
  echo "guardrail: captured local parser output must contain CGET/CSET" >&2
  exit 1
fi

LUA_PATH=$LUA_PATH_GUARD "$ROOT/src/luajit" -bl -e '
local function f()
  return f
end
return f
' >"$OUT"

if ! rg -q 'CNEW' "$OUT" || ! rg -q 'CSET' "$OUT"; then
  echo "guardrail: self-captured local function must use CNEW/CSET" >&2
  exit 1
fi

LUA_PATH=$LUA_PATH_GUARD "$ROOT/src/luajit" -e '
local dumped = string.dump(function()
  local x = 0
  return function()
    x = x + 1
    return x
  end
end)
local outer = assert(loadstring(dumped))
local inner = outer()
assert(inner() == 1 and inner() == 2)
'

cd "$ROOT/tests/stock/test"
LUA_PATH=$LUA_PATH_GUARD "$ROOT/src/luajit" test.lua --quiet lang/upvalue
LUA_PATH=$LUA_PATH_GUARD "$ROOT/src/luajit" misc/uclo.lua
LUA_PATH=$LUA_PATH_GUARD "$ROOT/src/luajit" test.lua --quiet opt/fwd/upval.lua
LUA_PATH=$LUA_PATH_GUARD "$ROOT/src/luajit" test.lua --quiet lang/goto.lua

echo "M5 local-cell opcode substrate guard passed"
