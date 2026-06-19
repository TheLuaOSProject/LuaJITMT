#!/bin/sh
# Guard x64 JIT HREF table node/hmask load ordering.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -e '
jit.opt.start("hotloop=1")
local t, keys = {}, {}
for i = 1, 128 do
  local k = "dyn" .. i
  keys[i] = k
  t[k] = i
end
local sum = 0
for i = 1, 800 do
  local k = keys[(i % 128) + 1]
  sum = sum + (t[k] or 0)
end
assert(sum > 0)
'

for needle in \
  'TABNODE_HMASK_OFS' \
  'TABNODE_FLAGS_OFS' \
  'emit_rmro(as, XO_GROUP3, XOg_TEST, node, TABNODE_FLAGS_OFS);' \
  'Reg idx;' \
  'idx = ra_scratch(as, iallow);' \
  'emit_rr(as, XO_ARITH(XOg_ADD), dest|REX_GC64, idx);' \
  'emit_rmro(as, XO_MOV, idx, dest, TABNODE_HMASK_OFS);' \
  'emit_rmro(as, XO_ARITH(XOg_AND), idx, dest,' \
  'emit_rmro(as, XO_MOV, dest|REX_GC64, tab, offsetof(GCtab, node));'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_asm_x86.h"; then
    echo "guardrail: missing x64 JIT HREF node-order marker: $needle" >&2
    exit 1
  fi
done

HREF_RETIRING_GUARDS=$(rg -F 'asm_tabnode_retiring_guard(as, dest);' \
  "$ROOT/src/lj_asm_x86.h" | wc -l | tr -d ' ')
if [ "$HREF_RETIRING_GUARDS" -lt 2 ]; then
  echo "guardrail: dynamic HREF must guard both node-load paths against retiring generations" >&2
  exit 1
fi

for reject in \
  'emit_rmro(as, XO_ARITH(XOg_ADD), dest|REX_GC64, tab, offsetof(GCtab,node))' \
  'emit_rmro(as, XO_MOV, dest, tab, offsetof(GCtab, hmask))' \
  'offsetof(GCtab, hmask)' \
  'emit_rmro(as, XO_ARITH(XOg_AND), dest, tab, offsetof(GCtab, hmask))' \
  'emit_rmro(as, XO_ARITH(XOg_AND), dest, key, offsetof(GCstr, sid))'
do
  if rg -F -n "$reject" "$ROOT/src/lj_asm_x86.h"; then
    echo "guardrail: x64 JIT HREF must use the node-header hmask: $reject" >&2
    exit 1
  fi
done

echo "M5 JIT HREF node-header hmask guard passed"
