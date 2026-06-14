#!/bin/sh
# Guard the M6 helper-backed trace-local table store bridge.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

make -C "$ROOT/src" >/dev/null

for needle in \
  'lj_tab_storetv_forjit(lua_State *L, TValue *dst, cTValue *src)' \
  'lj_gc2_barrier_tv(L, dst);  /* M6: traced table store value barrier. */' \
  'IRCALL_lj_tab_storetv_forjit' \
  'asm_ahstore_forjit(ASMState *as, IRIns *ir)' \
  '#if defined(__linux__) && LJ_TARGET_X64' \
  'rec_idx_store_trace_local(jit_State *J, TRef tab)' \
  'M6: no shared/new HSTORE bridge.' \
  'M6: no shared/nil ASTORE bridge.'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_tab.c" "$ROOT/src/lj_tab.h" \
      "$ROOT/src/lj_ircall.h" "$ROOT/src/lj_asm_x86.h" \
      "$ROOT/src/lj_record.c"; then
    echo "guardrail: missing table-store helper marker: $needle" >&2
    exit 1
  fi
done

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  "$ROOT/src/luajit" -e '
local util = require("jit.util")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local h = { stable = 0 }
for i = 1, 200 do
  h.stable = i
end
assert(h.stable == 200)
assert(not util.traceinfo(1), "shared hash table store unexpectedly traced")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local a = { 0 }
for i = 1, 200 do
  a[1] = i
end
assert(a[1] == 200)
assert(not util.traceinfo(1), "shared array table store unexpectedly traced")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function hash_insert(n)
  local out = { stable = 0 }
  for i = 1, n do
    local t = {}
    t.stable = i
    out = t
  end
  return out
end
local hi = hash_insert(80)
assert(hi.stable == 80)
assert(not util.traceinfo(1), "trace-local hash insertion unexpectedly traced")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function array_insert(n)
  local out = { 0 }
  for i = 1, n do
    local t = {}
    t[1] = i
    out = t
  end
  return out
end
local ai = array_insert(80)
assert(ai[1] == 80)
assert(not util.traceinfo(1), "trace-local array insertion unexpectedly traced")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function phi_store(n)
  local a = { stable = 0 }
  local b = { stable = 0 }
  local t = a
  for i = 1, n do
    if i == 1 then t = a else t = b end
    t.stable = i
  end
  return a.stable + b.stable
end
assert(phi_store(80) == 81)
assert(not util.traceinfo(1), "PHI-carried table store unexpectedly traced")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local up = { stable = 0 }
local function upvalue_store(n)
  for i = 1, n do
    up.stable = i
  end
  return up.stable
end
assert(upvalue_store(80) == 80)
assert(not util.traceinfo(1), "upvalue-carried table store unexpectedly traced")
'

HASH_IR=$(mktemp "${TMPDIR:-/tmp}/lj-m6-hstore-ir.XXXXXX")
ARRAY_IR=$(mktemp "${TMPDIR:-/tmp}/lj-m6-astore-ir.XXXXXX")
trap 'rm -f "$HASH_IR" "$ARRAY_IR"' EXIT HUP INT TERM

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  "$ROOT/src/luajit" -jdump=ir -e '
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1", "-sink")
local util = require("jit.util")
local function run(n)
  local out = { stable = 0 }
  for i = 1, n do
    local t = { stable = 0 }
    t.stable = i
    out = t
  end
  return out
end
local out = run(40)
assert(out.stable == 40)
assert(util.traceinfo(1), "trace-local hash store did not trace")
' > "$HASH_IR"

if ! grep -q 'TDUP' "$HASH_IR" || ! grep -q 'HSTORE' "$HASH_IR" ||
   ! grep -q 'XPOLL' "$HASH_IR"; then
  echo "guardrail: trace-local hash store must record TDUP/HSTORE with XPOLL" >&2
  cat "$HASH_IR" >&2
  exit 1
fi

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  "$ROOT/src/luajit" -jdump=ir -e '
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1", "-sink")
local util = require("jit.util")
local function run(n)
  local out = { 0 }
  for i = 1, n do
    local t = { 0 }
    t[1] = i
    out = t
  end
  return out
end
local out = run(40)
assert(out[1] == 40)
assert(util.traceinfo(1), "trace-local array store did not trace")
' > "$ARRAY_IR"

if ! grep -q 'TDUP' "$ARRAY_IR" || ! grep -q 'ASTORE' "$ARRAY_IR" ||
   ! grep -q 'XPOLL' "$ARRAY_IR"; then
  echo "guardrail: trace-local array store must record TDUP/ASTORE with XPOLL" >&2
  cat "$ARRAY_IR" >&2
  exit 1
fi

echo "M6 JIT table-store helper guard passed"
