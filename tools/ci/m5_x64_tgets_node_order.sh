#!/bin/sh
# Guard x64 TGETS node-header hmask loads and TSETS hash-store demotion.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
OUT=${TMPDIR:-/tmp}/lj_t-x64-tgets-forward

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

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-x64-tgets-forward.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
"$OUT"

for needle in \
  '|->BC_TGETS_Z:' \
  '|  mov r8, TAB:RB->node' \
  '|  mov r9d, dword [r8+TABNODE_FLAGS_OFS]' \
  '|  test r9d, TABNODE_FLAG_RETIRING' \
  '|  jnz ->vmeta_tgets' \
  '|  mov TMPRd, dword [r8+TABNODE_HMASK_OFS]' \
  '|  add NODE:TMPR, r8' \
  '|  mov64 r9, LJ_TFORWARD_BITS' \
  '|  cmp ITYPE, r9' \
  '|  je ->vmeta_tgets' \
  '|->BC_TSETS_Z:' \
  '|  jmp ->vmeta_tsets		// M5: no legacy x64 hash-slot store.' \
  't-x64-tgets-forward OK'
do
  if ! rg -F -q "$needle" "$ROOT/src/vm_x64.dasc" \
      "$ROOT/tests/t-x64-tgets-forward.c"; then
    echo "guardrail: missing x64 TGETS/TSETS marker: $needle" >&2
    exit 1
  fi
done

if ! awk '
  /[|]->BC_TGETS_Z:/ { in_get = 1; saw_node = saw_hmask = 0 }
  in_get && /mov r8, TAB:RB->node/ { saw_node = 1 }
  in_get && /mov r9d, dword \[r8\+TABNODE_FLAGS_OFS\]/ { saw_flags = 1; if (!saw_node) bad = 1 }
  in_get && /test r9d, TABNODE_FLAG_RETIRING/ { saw_test = 1; if (!saw_flags) bad = 1 }
  in_get && /jnz ->vmeta_tgets/ { saw_retire = 1; if (!saw_test) bad = 1 }
  in_get && /mov TMPRd, dword \[r8\+TABNODE_HMASK_OFS\]/ { saw_hmask = 1; if (!saw_node) bad = 1 }
  in_get && /mov64 r9, LJ_TFORWARD_BITS/ { forward = 1 }
  in_get && /cmp ITYPE, r9/ { forward_cmp = 1 }
  in_get && /je ->vmeta_tgets/ { forward_branch = 1 }
  in_get && /TAB:RB->hmask/ { bad = 1 }
  in_get && /case BC_TGETB:/ { in_get = 0 }
  /[|]->BC_TSETS_Z:/ { in_set = 1; checked_set = 1; next }
  in_set && /jmp ->vmeta_tsets/ { in_set = 0 }
  in_set && /mov \[TMPR\], ITYPE/ { bad = 1 }
  END { if (!checked_set || in_set || !saw_retire ||
	    !forward || !forward_cmp || !forward_branch) bad = 1; exit bad ? 1 : 0 }
' "$ROOT/src/vm_x64.dasc"; then
  echo "guardrail: x64 TGETS must use node headers, reject FORWARD, and TSETS must slow-path" >&2
  exit 1
fi

echo "M5 x64 TGETS node-header and TSETS slow-path guard passed"
