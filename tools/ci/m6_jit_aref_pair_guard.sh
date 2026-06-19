#!/bin/sh
# Guard the M6 x64 shared-array AREF generation pairing bridges.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TMP=${TMPDIR:-/tmp}/lj-m6-aref-pair.$$
TMP_COLO=${TMPDIR:-/tmp}/lj-m6-aref-pair-colo.$$
TMP_SPLIT=${TMPDIR:-/tmp}/lj-m6-aref-pair-split.$$
TMP_MISS=${TMPDIR:-/tmp}/lj-m6-aref-pair-miss.$$
trap 'rm -f "$TMP" "$TMP_COLO" "$TMP_SPLIT" "$TMP_MISS"' EXIT

make -C "$ROOT/src" >/dev/null

for needle in \
  'rec_idx_tab_trace_local(jit_State *J, TRef tab)' \
  'rec_idx_tab_array_has_hdr(const GCtab *t, const TValue *array)' \
  'array == coloarray' \
  'lj_tab_array_snapshot_acq(t, &record_array)' \
  'rec_idx_tab_array_has_hdr(t, record_array)' \
  'rec_idx_array_hdr_asize(jit_State *J, TRef arrayref)' \
  'rec_idx_array_hdr_guards(jit_State *J, TRef tab, TRef arrayref)' \
  'rec_idx_array_asize_ref(jit_State *J, GCtab *t, TRef tab,' \
  'emitir(IRTG(IR_NE, IRT_PGC), arrayref, lj_ir_kptr(J, NULL));' \
  'lj_ir_kintpgc(J, sizeof(GCtab))' \
  'M6: shared separated AREF pairs slots with TabArrayHdr.asize.' \
  'M6: shared separated non-array bounds use TabArrayHdr.asize.' \
  'asizeref = rec_idx_array_asize_ref(J, t, ix->tab, record_array,' \
  'emitir(IRTI(IR_XLOAD), hdrref, 0);' \
  'M6: legacy shared AREF guards TAB_ARRAY pair stability.' \
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
    exit !(array >= 2 && hdradd >= 2 && xload >= 2 && asize == 0 &&
	    eq == 0 && aref && aload && xpoll)
  }
  inir && /FLOAD .*tab[.]array/ { array++ }
  inir && / p64 ADD / && /-16/ { hdradd++ }
  inir && / XLOAD / { xload++ }
  inir && /FLOAD .*tab[.]asize/ { asize++ }
  inir && / p64 EQ / { eq++ }
  inir && / AREF / { aref = 1 }
  inir && / ALOAD / { aload = 1 }
  inir && / XPOLL / { xpoll = 1 }
  END { if (!done) exit 1 }
' "$TMP"; then
  cat "$TMP" >&2
  echo "guardrail: separated shared array reads must load bounds from TabArrayHdr" >&2
  exit 1
fi

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  timeout 20s "$ROOT/src/luajit" -jdump=ir -e '
    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    jit.off()
    local t = { 1, 2, 3, 4 }
    for i = 5, 64 do t[i] = i end
    jit.on()
    local s = 0
    for i = 1, 80 do
      local k = (i % 64) + 1
      s = s + (t[k] or 0)
    end
    assert(s > 0)
  ' >"$TMP_SPLIT" 2>&1

if ! awk '
  /---- TRACE 1 IR/ { inir = 1; next }
  /---- TRACE 1 stop/ {
    done = 1
    exit !(array >= 2 && hdradd >= 2 && xload >= 2 && asize == 0 &&
	    eq == 0 && aref && aload && xpoll)
  }
  inir && /FLOAD .*tab[.]array/ { array++ }
  inir && / p64 ADD / && /-16/ { hdradd++ }
  inir && / XLOAD / { xload++ }
  inir && /FLOAD .*tab[.]asize/ { asize++ }
  inir && / p64 EQ / { eq++ }
  inir && / AREF / { aref = 1 }
  inir && / ALOAD / { aload = 1 }
  inir && / XPOLL / { xpoll = 1 }
  END { if (!done) exit 1 }
' "$TMP_SPLIT"; then
  cat "$TMP_SPLIT" >&2
  echo "guardrail: split-from-colocated arrays must use header bounds after publish" >&2
  exit 1
fi

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  timeout 20s "$ROOT/src/luajit" -jdump=ir -e '
    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    jit.off()
    local t = { 1, 2, 3, 4 }
    jit.on()
    local s = 0
    for i = 1, 80 do
      local k = (i % 4) + 1
      s = s + (t[k] or 0)
    end
    assert(s > 0)
  ' >"$TMP_COLO" 2>&1

if ! awk '
  /---- TRACE 1 IR/ { inir = 1; next }
  /---- TRACE 1 stop/ {
    done = 1
    exit !(array >= 4 && asize >= 2 && eq >= 2 && xload == 0 &&
	    aref && aload && xpoll)
  }
  inir && /FLOAD .*tab[.]array/ { array++ }
  inir && /FLOAD .*tab[.]asize/ { asize++ }
  inir && / p64 EQ / { eq++ }
  inir && / XLOAD / { xload++ }
  inir && / AREF / { aref = 1 }
  inir && / ALOAD / { aload = 1 }
  inir && / XPOLL / { xpoll = 1 }
  END { if (!done) exit 1 }
' "$TMP_COLO"; then
  cat "$TMP_COLO" >&2
  echo "guardrail: colocated shared array reads must keep the legacy pair guard" >&2
  exit 1
fi

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
      local k = 160 + (i % 2)
      if t[k] == nil then s = s + 1 end
    end
    assert(s == 80)
  ' >"$TMP_MISS" 2>&1

if ! awk '
  /---- TRACE 1 IR/ { inir = 1; next }
  /---- TRACE 1 stop/ {
    done = 1
    exit !(array >= 2 && hdradd >= 2 && xload >= 2 && asize == 0 &&
	    ule && href && !aref && xpoll)
  }
  inir && /FLOAD .*tab[.]array/ { array++ }
  inir && / p64 ADD / && /-16/ { hdradd++ }
  inir && / XLOAD / { xload++ }
  inir && /FLOAD .*tab[.]asize/ { asize++ }
  inir && / ULE / { ule = 1 }
  inir && / HREF / { href = 1 }
  inir && / AREF / { aref = 1 }
  inir && / XPOLL / { xpoll = 1 }
  END { if (!done) exit 1 }
' "$TMP_MISS"; then
  cat "$TMP_MISS" >&2
  echo "guardrail: separated shared out-of-array guards must load bounds from TabArrayHdr" >&2
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

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  timeout 20s "$ROOT/src/luajit" -e '
    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    local keep = {}
    for i = 1, 120 do
      local t = {}
      for j = 1, 80 do
        t[j] = "value-" .. i .. "-" .. j
      end
      keep[i] = t
    end
    assert(#keep == 120 and keep[120][80] == "value-120-80")
  '

if ! rg -F -q 'm6_jit_aref_pair_guard.sh' "$ROOT/tools/ci/m6_jit.sh"; then
  echo "guardrail: m6_jit_aref_pair_guard.sh is not wired into M6 aggregate" >&2
  exit 1
fi

echo "M6 JIT shared AREF generation-pair guard passed"
