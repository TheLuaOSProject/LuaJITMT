#!/bin/sh
# Guard the M6 x64 HREFK node-header slot guard bridge.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

make -C "$ROOT/src" >/dev/null

for needle in \
  'x64 HREFK guards the constant slot against the loaded node header' \
  'emit_gmroi(as, XG_ARITHi(XOg_CMP), node, -(int32_t)sizeof(TabNodeHdr)' \
  '#if !(defined(__linux__) && LJ_TARGET_X64)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_record.c" "$ROOT/src/lj_asm_x86.h"; then
    echo "guardrail: missing HREFK node-header marker: $needle" >&2
    exit 1
  fi
done

IR_DUMP=$(mktemp "${TMPDIR:-/tmp}/lj-m6-hrefk-ir.XXXXXX")
trap 'rm -f "$IR_DUMP"' EXIT HUP INT TERM

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  "$ROOT/src/luajit" -jdump=ir -e '
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local t = { foo = 1, bar = 2, baz = 3 }
local s = 0
for i = 1, 60 do
  s = s + t.foo
end
assert(s == 60)
' > "$IR_DUMP" 2>&1

if ! awk '
  /---- TRACE 1 IR/ { inir = 1; next }
  inir && /---- TRACE 1 stop/ {
    exit !(node && hrefk && xpoll && !hmask)
  }
  inir && /tab.node/ { node = 1 }
  inir && /HREFK/ { hrefk = 1 }
  inir && /XPOLL/ { xpoll = 1 }
  inir && /tab.hmask/ { hmask = 1 }
  END { if (!inir) exit 1 }
' "$IR_DUMP"; then
  echo "guardrail: TRACE 1 HREFK must use tab.node without tab.hmask mirror guard" >&2
  cat "$IR_DUMP" >&2
  exit 1
fi

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  "$ROOT/src/luajit" -e '
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local t = { foo = 1, bar = 2, baz = 3 }
local function run(n)
  local s = 0
  for i = 1, n do
    s = s + t.foo
  end
  return s
end
assert(run(80) == 80)
for i = 1, 2000 do
  t["resize_" .. i] = i
end
t.foo = 7
assert(run(80) == 560)
'

echo "M6 JIT HREFK node-header guard passed"
