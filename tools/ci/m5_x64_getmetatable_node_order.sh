#!/bin/sh
# Guard x64 getmetatable __metatable probe node-before-hmask ordering.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff -e '
local token = {}
local t = setmetatable({}, { __metatable = token })
assert(getmetatable(t) == token)
local u = setmetatable({}, {})
assert(type(getmetatable(u)) == "table")
'

for needle in \
  '|.ffunc_1 getmetatable' \
  '|  mov r8, TAB:RB->node' \
  '|  mov RAd, dword [r8-8]' \
  '|  add NODE:RA, r8'
do
  if ! rg -F -q "$needle" "$ROOT/src/vm_x64.dasc"; then
    echo "guardrail: missing x64 getmetatable node-order marker: $needle" >&2
    exit 1
  fi
done

if ! awk '
  /[|][.]ffunc_1 getmetatable/ { infn = 1; saw_node = 0 }
  infn && /mov r8, TAB:RB->node/ { saw_node = 1 }
  infn && /mov RAd, dword \[r8-8\]/ { if (!saw_node) bad = 1 }
  infn && /TAB:RB->hmask/ { bad = 1 }
  infn && /[|][.]ffunc_2 setmetatable/ { infn = 0 }
  END { exit bad ? 1 : 0 }
' "$ROOT/src/vm_x64.dasc"; then
  echo "guardrail: x64 getmetatable must load hmask from the node header" >&2
  exit 1
fi

echo "M5 x64 getmetatable node-header hmask guard passed"
