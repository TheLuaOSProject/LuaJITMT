#!/bin/sh
# Guard the M6 x64 shared-array AREF pair-stability bridge.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TMP=${TMPDIR:-/tmp}/lj-m6-aref-pair.$$
trap 'rm -f "$TMP"' EXIT

make -C "$ROOT/src" >/dev/null

for needle in \
  'rec_idx_tab_trace_local(jit_State *J, TRef tab)' \
  'M6: shared AREF guards TAB_ARRAY pair stability until AHdr lands.' \
  'emitir(IRTG(IR_EQ, IRT_PGC), arrayref2, arrayref);'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_record.c"; then
    echo "guardrail: missing shared AREF pair marker: $needle" >&2
    exit 1
  fi
done

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  timeout 20s "$ROOT/src/luajit" -jdump=ir -e '
    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    jit.off()
    local t = {}
    for i = 1, 128 do t[i] = i end
    jit.on()
    local s = 0
    for i = 1, 80 do
      local k = (i % 128) + 1
      s = s + (t[k] or 0)
    end
    assert(s > 0)
  ' >"$TMP" 2>&1

if ! awk '
  /---- TRACE 1 IR/ { inir = 1; next }
  /---- TRACE 1 stop/ {
    done = 1
    exit !(array >= 4 && asize >= 2 && eq >= 2 && aref && aload && xpoll)
  }
  inir && /FLOAD .*tab[.]array/ { array++ }
  inir && /FLOAD .*tab[.]asize/ { asize++ }
  inir && / p64 EQ / { eq++ }
  inir && / AREF / { aref = 1 }
  inir && / ALOAD / { aload = 1 }
  inir && / XPOLL / { xpoll = 1 }
  END { if (!done) exit 1 }
' "$TMP"; then
  cat "$TMP" >&2
  echo "guardrail: shared array reads must guard paired TAB_ARRAY loads" >&2
  exit 1
fi

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  timeout 20s "$ROOT/src/luajit" -e '
    local util = require("jit.util")
    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    jit.off()
    local t = {}
    for i = 1, 32 do t[i] = i end
    jit.on()
    local idx = 1
    local function read(n)
      local s = 0
      for _ = 1, n do
        s = s + (t[idx] or 0)
      end
      return s
    end
    assert(read(80) == 80)
    assert(util.traceinfo(1), "shared array read did not trace")
    jit.off()
    for i = 33, 128 do t[i] = i end
    jit.on()
    idx = 64
    assert(read(80) == 5120)
    assert(util.traceinfo(1), "trace missing after array grow")
  '

if ! rg -F -q 'm6_jit_aref_pair_guard.sh' "$ROOT/tools/ci/m6_jit.sh"; then
  echo "guardrail: m6_jit_aref_pair_guard.sh is not wired into M6 aggregate" >&2
  exit 1
fi

echo "M6 JIT shared AREF pair guard passed"
