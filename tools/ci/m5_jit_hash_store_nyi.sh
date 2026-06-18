#!/bin/sh
# Guard helper-backed table stores and stale table-store NYI markers.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -e '
local util = require("jit.util")

local function traces()
  local n = 0
  for i = 1, 200 do
    if util.traceinfo(i) then n = n + 1 end
  end
  return n
end

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local h = { stable = 0 }
for i = 1, 200 do
  h.stable = i
end
assert(h.stable == 200)
assert(traces() > 0, "existing hash table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local a = { 0 }
for i = 1, 200 do
  a[1] = i
end
assert(a[1] == 200)
assert(traces() > 0, "existing array table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local hn = {}
for i = 1, 200 do
  hn["k" .. i] = i
end
assert(hn.k200 == 200)
assert(traces() > 0, "new string hash table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function array_insert(n)
  local out = { 0 }
  for i = 1, n do
    local an = {}
    an[1] = i
    out = an
  end
  return out
end
local an = array_insert(80)
assert(an[1] == 80)
assert(traces() > 0, "fresh array slot table store did not trace")
'

if rg -n 'M6: numeric NEWREF/HSTORE uses the generic returned-slot helper' "$ROOT/src/lj_record.c" >/dev/null &&
   rg -n 'M6: previous-nil in-bounds ASTORE/HSTORE uses the helper bridge' "$ROOT/src/lj_record.c" >/dev/null; then
  :
else
  echo "guardrail: missing table-store bridge marker" >&2
  exit 1
fi

if rg -n -e 'M6: no new/nil HSTORE bridge' \
    -e 'M6: no new HSTORE bridge' \
    -e 'M6: no numeric new HSTORE bridge' \
    -e 'M6: no nil ASTORE bridge' "$ROOT/src/lj_record.c" >/dev/null; then
  echo "guardrail: stale table-store NYI marker remains" >&2
  exit 1
fi

echo "M5 JIT table-store bridge guard passed"
