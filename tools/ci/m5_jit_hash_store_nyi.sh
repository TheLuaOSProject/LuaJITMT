#!/bin/sh
# Guard that legacy table slot stores are not recorded before NHdr/AHdr writes.
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
assert(traces() == 0, "hash table store unexpectedly traced")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local a = { 0 }
for i = 1, 200 do
  a[1] = i
end
assert(a[1] == 200)
assert(traces() == 0, "array table store unexpectedly traced")
'

if ! awk '
  /} else {  \/\* Indexed store\. \*\// {
    in_store = 1
    saw_hash = saw_hash_nyi = saw_array = saw_array_nyi = 0
  }
  in_store && /loadop == IR_HLOAD/ { saw_hash = 1 }
  in_store && /M5: no legacy hash HSTORE/ { saw_hash_nyi = 1 }
  in_store && /loadop == IR_ALOAD/ { saw_array = 1 }
  in_store && /M5: no legacy array ASTORE/ { saw_array_nyi = 1 }
  in_store && /Convert int to number before storing/ {
    if (!saw_hash || !saw_hash_nyi || !saw_array || !saw_array_nyi)
      bad = 1
    checked = 1
    in_store = 0
  }
  END { exit checked && !bad ? 0 : 1 }
' "$ROOT/src/lj_record.c"; then
  echo "guardrail: recorder must reject legacy hash and array table stores" >&2
  exit 1
fi

if rg -n 'M5: no legacy hash HSTORE' "$ROOT/src/lj_record.c" >/dev/null &&
   rg -n 'M5: no legacy array ASTORE' "$ROOT/src/lj_record.c" >/dev/null; then
  :
else
  echo "guardrail: missing table-store NYI marker" >&2
  exit 1
fi

echo "M5 JIT table-store NYI guard passed"
