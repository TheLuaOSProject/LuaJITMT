#!/bin/sh
# Guard CGET/CSET recording under the M6 x64 JIT bridge.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
LUA_PATH_GUARD="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;"
DUMP=${TMPDIR:-/tmp}/lj_m6_jit_cell_ops.$$
trap 'rm -f "$DUMP"' EXIT HUP INT TERM

need_marker()
{
  needle=$1
  shift
  if ! rg -F -q "$needle" "$@"; then
    echo "guardrail: missing CGET/CSET JIT marker: $needle" >&2
    exit 1
  fi
}

need_dump()
{
  pattern=$1
  desc=$2
  if ! rg -q "$pattern" "$DUMP"; then
    echo "guardrail: missing $desc in CGET/CSET IR dump" >&2
    sed -n '1,160p' "$DUMP" >&2
    exit 1
  fi
}

need_marker 'case BC_CGET:' "$ROOT/src/lj_record.c"
need_marker 'case BC_CSET:' "$ROOT/src/lj_record.c"
need_marker 'rec_celluv(jit_State *J' "$ROOT/src/lj_record.c"
need_marker 'IRT(IR_UREFC, IRT_PGC), slotref' "$ROOT/src/lj_record.c"
need_marker 'emitir(IRTG(IR_ULOAD' "$ROOT/src/lj_record.c"
need_marker 'emitir(IRT(IR_USTORE' "$ROOT/src/lj_record.c"
need_marker 'emitir(IRT(IR_OBAR' "$ROOT/src/lj_record.c"
need_marker 'irt_isp32(IR(ir->op1)->t)' "$ROOT/src/lj_asm_x86.h"
need_marker 'cellops & BCREAD_CELL_CNEW' "$ROOT/src/lj_bcread.c"
need_marker 'pt->flags |= PROTO_NOJIT' "$ROOT/src/lj_bcread.c"

LUA_PATH=$LUA_PATH_GUARD "$ROOT/src/luajit" -jdump=i -e '
jit.opt.start("hotloop=1", "hotexit=1")
local function run(n)
  local x = 0
  local function touch() return x end
  for i = 1, n do x = x + 1 end
  return x, touch
end
local v, f = run(200)
assert(v == 200 and f() == 200)
' >"$DUMP"

need_dump 'TRACE 1 stop -> loop' 'owner numeric trace'
need_dump 'UREFC' 'owner numeric UREFC'
need_dump 'ULOAD' 'owner numeric ULOAD'
need_dump 'USTORE' 'owner numeric USTORE'

LUA_PATH=$LUA_PATH_GUARD "$ROOT/src/luajit" -jdump=i -e '
local pool = { "even", "odd" }
jit.opt.start("hotloop=1", "hotexit=1")
local function run(n)
  local x = pool[1]
  local function get() return x end
  for i = 1, n do x = pool[(i % 2) + 1] end
  return get()
end
assert(run(200) == pool[1])
' >"$DUMP"

need_dump 'TRACE 1 stop -> loop' 'owner GC-valued trace'
need_dump 'UREFC' 'owner GC-valued UREFC'
need_dump 'USTORE' 'owner GC-valued USTORE'
need_dump 'OBAR' 'owner GC-valued OBAR'

LUA_PATH=$LUA_PATH_GUARD "$ROOT/src/luajit" -jdump=i -e '
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local src = function(n)
  local x = 0
  local function touch() return x end
  for i = 1, n do x = x + 1 end
  return x, touch
end
local run = assert(loadstring(string.dump(src)))
local v, f = run(200)
assert(v == 200 and f() == 200)
' >"$DUMP"

need_dump 'TRACE 1 stop -> loop' 'loaded v4 CGET/CSET trace'
need_dump 'UREFC' 'loaded v4 CGET/CSET UREFC'
need_dump 'ULOAD' 'loaded v4 CGET/CSET ULOAD'
need_dump 'USTORE' 'loaded v4 CGET/CSET USTORE'

LUA_PATH=$LUA_PATH_GUARD "$ROOT/src/luajit" -e '
local util = require"jit.util"
jit.flush()
jit.opt.start("hotloop=1")
local src = function(n)
  local keep
  for i = 1, n do
    local function f() return f end
    keep = f
  end
  return keep
end
local run = assert(loadstring(string.dump(src)))
local f = run(30)
assert(f() == f)
assert(not util.traceinfo(1), "loaded CNEW creation should remain PROTO_NOJIT")
'

echo "M6 JIT CGET/CSET guard passed"
