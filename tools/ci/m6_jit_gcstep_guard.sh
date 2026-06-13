#!/bin/sh
# Guard legacy JIT GC-step pacing until independent GC2 trace pacing replaces it.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
DUMP=${TMPDIR:-/tmp}/lj_t-jit-gcstep.dump

make -C "$ROOT/src" >/dev/null

for needle in \
  'lj_gc_step_jit' \
  'IR_GCSTEP' \
  'asm_gcstep' \
  'asm_gc_check' \
  'as->gcsteps++'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc.h" \
      "$ROOT/src/lj_ir.h" "$ROOT/src/lj_ircall.h" "$ROOT/src/lj_snap.c" \
      "$ROOT/src/lj_asm.c" "$ROOT/src/lj_asm_x86.h"; then
    echo "guardrail: missing JIT GC-step pacing marker: $needle" >&2
    exit 1
  fi
done

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  timeout 20s "$ROOT/src/luajit" -jdump=ir -e \
  'jit.opt.start("hotloop=1","hotexit=1"); local x; for i=1,100 do x={} end; assert(type(x)=="table")' \
  >"$DUMP"
if ! rg -q 'GCSTEP' "$DUMP"; then
  echo "guardrail: sunk allocation replay must still emit IR_GCSTEP" >&2
  exit 1
fi

timeout 20s "$ROOT/src/luajit" "$ROOT/tests/stock/test/misc/gcstep.lua"

if ! rg -F -q 'm6_jit_gcstep_guard.sh' "$ROOT/tools/ci/m6_jit.sh"; then
  echo "guardrail: m6_jit_gcstep_guard.sh is not wired into the M6 aggregate" >&2
  exit 1
fi

echo "M6 JIT GC-step guard passed"
