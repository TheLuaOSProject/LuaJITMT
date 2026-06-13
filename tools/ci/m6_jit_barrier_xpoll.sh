#!/bin/sh
# Guard x64 trace barriers across XPOLL poll regions.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TBAR_DUMP=${TMPDIR:-/tmp}/lj_t-jit-tbar-xpoll.dump
OBAR_DUMP=${TMPDIR:-/tmp}/lj_t-jit-obar-xpoll.dump

make -C "$ROOT/src" >/dev/null

for needle in \
  'xpoll_barrier(J, ref)' \
  'trace_barrier(J, ref)' \
  'trace_barrier(J, tref_ref(tr))' \
  'fins->op1 < J->chain[IR_XPOLL]' \
  'DISPATCH_TG(mark_active)' \
  'IRCALL_lj_gc2_barrier_tab_g' \
  'IRCALL_lj_gc_pubuv' \
  'LJ_GC_BLACK' \
  'LJ_GC_WHITES'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_opt_fold.c" \
      "$ROOT/src/lj_asm_x86.h"; then
    echo "guardrail: missing XPOLL barrier marker: $needle" >&2
    exit 1
  fi
done

if ! rg -F -q 'm6_jit_barrier_xpoll.sh' "$ROOT/tools/ci/m6_jit.sh"; then
  echo "guardrail: m6_jit_barrier_xpoll.sh is not wired into the M6 aggregate" >&2
  exit 1
fi

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  timeout 20s "$ROOT/src/luajit" -jdump=im -e \
  'jit.opt.start("hotloop=1","hotexit=1"); local t={}; local mts={}; for i=1,80 do mts[i]={} end; for i=1,64 do setmetatable(t, mts[i]) end' \
  >"$TBAR_DUMP"

if ! awk '
  /---- TRACE 1 mcode/ { done = 1; exit !(xp && fs && tb) }
  /------ LOOP/ { loop = 1; next }
  loop && /XPOLL/ { xp = 1 }
  loop && /FSTORE/ { fs = 1 }
  loop && /TBAR/ { tb = 1 }
  END { if (!done) exit !(xp && fs && tb) }
' "$TBAR_DUMP"; then
  echo "guardrail: setmetatable loop must emit FSTORE+TBAR after XPOLL" >&2
  exit 1
fi

if ! awk '
  /->LOOP:/ { loop = 1; next }
  loop && /cmp dword \[r14\+0x[0-9a-f]+\], \+0x00/ { cmp++ }
  loop && /lj_gc2_barrier_tab_g/ { call = 1; done = 1; exit !(cmp >= 2) }
  END { if (!done) exit 1 }
' "$TBAR_DUMP"; then
  echo "guardrail: post-XPOLL TBAR must lower to poll+mark checks and GC2 call" >&2
  exit 1
fi

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  timeout 20s "$ROOT/src/luajit" -jdump=im -e \
  'jit.opt.start("hotloop=1","hotexit=1"); local uv; local vals={}; for i=1,80 do vals[i]={} end; local function f() for i=1,64 do uv=vals[i] end end; f(); assert(uv==vals[64])' \
  >"$OBAR_DUMP"

if ! awk '
  /---- TRACE 1 mcode/ { done = 1; exit !(xp && us && ob) }
  /------ LOOP/ { loop = 1; next }
  loop && /XPOLL/ { xp = 1 }
  loop && /USTORE/ { us = 1 }
  loop && /OBAR/ { ob = 1 }
  END { if (!done) exit !(xp && us && ob) }
' "$OBAR_DUMP"; then
  echo "guardrail: upvalue loop must emit USTORE+OBAR after XPOLL" >&2
  exit 1
fi

if ! awk '
  /->LOOP:/ { loop = 1; next }
  loop && /test byte/ { test++ }
  loop && /cmp dword \[r14\+0x[0-9a-f]+\], \+0x00/ { cmp++ }
  loop && /lj_gc_pubuv/ { done = 1; exit !(test >= 2 && cmp >= 2) }
  END { if (!done) exit 1 }
' "$OBAR_DUMP"; then
  echo "guardrail: post-XPOLL OBAR must lower to legacy tests, poll+mark checks and pubuv call" >&2
  exit 1
fi

echo "M6 JIT XPOLL barrier guard passed"
