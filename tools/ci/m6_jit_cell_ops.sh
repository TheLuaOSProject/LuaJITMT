#!/bin/sh
# Guard local-cell recording under the M6 x64 JIT bridge.
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
    echo "guardrail: missing local-cell JIT marker: $needle" >&2
    exit 1
  fi
}

need_dump()
{
  pattern=$1
  desc=$2
  if ! rg -q "$pattern" "$DUMP"; then
    echo "guardrail: missing $desc in local-cell IR dump" >&2
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
need_marker 'case BC_CNEW:' "$ROOT/src/lj_record.c"
need_marker 'case BC_FNEW:' "$ROOT/src/lj_record.c"
need_marker 'rec_fnew_celluv(jit_State *J' "$ROOT/src/lj_record.c"
need_marker 'rec_fnew_promoted_slots(jit_State *J' "$ROOT/src/lj_record.c"
need_marker 'rec_celluv_promote_pending(J)' "$ROOT/src/lj_record.c"
need_marker 'IRCALL_lj_func_newuvcell_forjit' "$ROOT/src/lj_record.c" "$ROOT/src/lj_ircall.h"
need_marker 'IRCALL_lj_func_newL_gc_forjit' "$ROOT/src/lj_record.c" "$ROOT/src/lj_ircall.h"
need_marker 'IRCALL_lj_func_syncslot_forjit' "$ROOT/src/lj_record.c" "$ROOT/src/lj_ircall.h"
need_marker 'lj_func_newuvcell_forjit' "$ROOT/src/lj_func.c" "$ROOT/src/lj_func.h"
need_marker 'lj_func_newL_gc_forjit' "$ROOT/src/lj_func.c" "$ROOT/src/lj_func.h"
need_marker 'lj_func_syncslot_forjit' "$ROOT/src/lj_func.c" "$ROOT/src/lj_func.h"
need_marker 'irt_isp32(IR(ir->op1)->t)' "$ROOT/src/lj_asm_x86.h"
need_marker 'IR(ir->op1)->o == IR_SLOAD' "$ROOT/src/lj_asm_x86.h"
need_marker 'emit_shifti(as, XOg_SHR|REX_64' "$ROOT/src/lj_asm_x86.h"
need_marker 'irt_type(IR(ir->op1)->t) == IRT_PGC' "$ROOT/src/lj_asm_x86.h"
need_marker 'cellops |= BCREAD_CELL_CNEW' "$ROOT/src/lj_bcread.c"

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

LUA_PATH=$LUA_PATH_GUARD "$ROOT/src/luajit" -jdump=i -e '
local util = require"jit.util"
jit.flush()
jit.opt.start("hotloop=1")
local function run(n)
  local keep
  for i = 1, n do
    local function f() return f end
    keep = f
  end
  return keep
end
local f = run(30)
assert(f() == f)
assert(util.traceinfo(1), "source CNEW/FNEW creation should trace")
' >"$DUMP"

need_dump 'CALLS.*lj_func_newuvcell_forjit' 'source CNEW helper call'
need_dump 'CALLA.*lj_func_newL_gc_forjit' 'source FNEW helper call'
need_dump 'UREFC' 'source CNEW/FNEW UREFC'
need_dump 'USTORE' 'source CNEW/FNEW USTORE'
need_dump 'OBAR' 'source CNEW/FNEW OBAR'

LUA_PATH=$LUA_PATH_GUARD "$ROOT/src/luajit" -jdump=i -e '
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
assert(util.traceinfo(1), "loaded CNEW/FNEW creation should trace")
' >"$DUMP"

need_dump 'CALLS.*lj_func_newuvcell_forjit' 'loaded CNEW helper call'
need_dump 'CALLA.*lj_func_newL_gc_forjit' 'loaded FNEW helper call'
need_dump 'UREFC' 'loaded CNEW/FNEW UREFC'
need_dump 'USTORE' 'loaded CNEW/FNEW USTORE'
need_dump 'OBAR' 'loaded CNEW/FNEW OBAR'

LUA_PATH=$LUA_PATH_GUARD "$ROOT/src/luajit" -jdump=i -e '
local util = require"jit.util"
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function run(n)
  local x = 1
  local keep
  for i = 1, n do
    local function f() return f, x end
    keep = f
  end
  return keep
end
local f = run(30)
local self, x = f()
assert(self == f and x == 1)
assert(util.traceinfo(1), "source mixed raw-local CNEW/FNEW should trace")
' >"$DUMP"

need_dump 'TRACE 1 stop -> loop' 'source mixed raw-local FNEW trace'
need_dump 'TMPREF' 'source mixed raw-local TMPREF'
need_dump 'CALLS.*lj_func_syncslot_forjit' 'source mixed raw-local sync helper'
need_dump 'CALLA.*lj_func_newL_gc_forjit' 'source mixed raw-local FNEW helper'
need_dump 'UREFC' 'source mixed raw-local UREFC'
need_dump 'USTORE' 'source mixed raw-local USTORE'

LUA_PATH=$LUA_PATH_GUARD "$ROOT/src/luajit" -jdump=i -e '
local util = require"jit.util"
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local src = function(n)
  local x = 1
  local keep
  for i = 1, n do
    local function f() return f, x end
    keep = f
  end
  return keep
end
local run = assert(loadstring(string.dump(src)))
local f = run(30)
local self, x = f()
assert(self == f and x == 1)
assert(util.traceinfo(1), "loaded mixed raw-local CNEW/FNEW should trace")
' >"$DUMP"

need_dump 'TRACE 1 stop -> loop' 'loaded mixed raw-local FNEW trace'
need_dump 'CALLS.*lj_func_syncslot_forjit' 'loaded mixed raw-local sync helper'
need_dump 'CALLA.*lj_func_newL_gc_forjit' 'loaded mixed raw-local FNEW helper'

LUA_PATH=$LUA_PATH_GUARD "$ROOT/src/luajit" -e '
local util = require"jit.util"
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function run(n)
  local x = 0
  local keep
  for i = 1, n do
    x = x + 1
    local function f() return f, x end
    keep = f
  end
  return keep
end
local f = run(30)
local self, x = f()
assert(self == f and x == 30)
assert(util.traceinfo(1), "pre-FNEW promoted local update should trace")
'

LUA_PATH=$LUA_PATH_GUARD "$ROOT/src/luajit" -e '
local util = require"jit.util"
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function run(n)
  local x = 0
  local keep
  for i = 1, n do
    local function f() return f, x end
    x = x + 1
    keep = f
  end
  return keep, x
end
local f, x = run(30)
local self, fx = f()
assert(self == f and fx == 30 and x == 30)
assert(util.traceinfo(1), "post-FNEW promoted local update should trace")
'

echo "M6 JIT local-cell guard passed"
