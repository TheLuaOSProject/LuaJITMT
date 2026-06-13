#!/bin/sh
# Guard x64 TGETS/TSETS node-before-hmask ordering.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff -e '
local t = { foo = 17 }
local sum = 0
for i = 1, 200 do
  sum = sum + t.foo
  t.bar = i
  assert(t.bar == i)
end
assert(sum == 200 * 17)
'

for needle in \
  '|->BC_TGETS_Z:' \
  '|  mov r8, TAB:RB->node' \
  '|  add NODE:TMPR, r8' \
  '|->BC_TSETS_Z:'
do
  if ! rg -F -q "$needle" "$ROOT/src/vm_x64.dasc"; then
    echo "guardrail: missing x64 TGETS/TSETS node-order marker: $needle" >&2
    exit 1
  fi
done

if ! awk '
  /[|]->BC_TGETS_Z:/ { in_get = 1; saw_node = saw_hmask = 0 }
  in_get && /mov r8, TAB:RB->node/ { saw_node = 1 }
  in_get && /mov TMPRd, TAB:RB->hmask/ { saw_hmask = 1; if (!saw_node) bad = 1 }
  in_get && /settp ITYPE, STR:RC, LJ_TSTR/ { in_get = 0 }
  /[|]->BC_TSETS_Z:/ { in_set = 1; saw_node = saw_hmask = 0 }
  in_set && /mov r8, TAB:RB->node/ { saw_node = 1 }
  in_set && /mov TMPRd, TAB:RB->hmask/ { saw_hmask = 1; if (!saw_node) bad = 1 }
  in_set && /settp ITYPE, STR:RC, LJ_TSTR/ { in_set = 0 }
  END { exit bad ? 1 : 0 }
' "$ROOT/src/vm_x64.dasc"; then
  echo "guardrail: x64 TGETS/TSETS must load node before hmask" >&2
  exit 1
fi

echo "M5 x64 TGETS/TSETS node-order guard passed"
