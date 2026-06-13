#!/bin/sh
# Guard GC2 allocation-pacing readiness while legacy JIT GCSTEP is still live.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
DUMP=${TMPDIR:-/tmp}/lj_t-jit-gc2-readiness.dump

make -C "$ROOT/src" >/dev/null

for needle in \
  'static void gc2_maybe_trigger_cycle(global_State *g)' \
  'lj_gc_threshold_load(g) == LJ_MAX_MEM' \
  'lj_gc_threshold_store(g, g->gc.total)' \
  'lj_gc2_assist(global_State *g, TGState *tg)' \
  'lj_gc2_assist(g, L2TG(L));  /* 05 section 5.11 trace-side assist bridge. */' \
  'la_cas32(&g->gc2.assist_active' \
  'gc2_drain_active_ssb_to_grey(global_State *g, TGState *tg' \
  'gc2_drain_published_ssb_to_grey(global_State *g' \
  'lj_gc_step_jit' \
  'IR_GCSTEP' \
  'asm_gc_check'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_gc2.c" "$ROOT/src/lj_gc2.h" \
      "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc.h" "$ROOT/src/lj_ir.h" \
      "$ROOT/src/lj_ircall.h" "$ROOT/src/lj_snap.c" \
      "$ROOT/src/lj_asm.c" "$ROOT/src/lj_asm_x86.h"; then
    echo "guardrail: missing GC2/JIT pacing readiness marker: $needle" >&2
    exit 1
  fi
done

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  timeout 20s "$ROOT/src/luajit" -jdump=ir -e \
  'jit.opt.start("hotloop=1","hotexit=1"); local x; for i=1,100 do x={} end; assert(type(x)=="table")' \
  >"$DUMP"

for needle in 'TNEW' 'XPOLL' 'GCSTEP'
do
  if ! rg -q "$needle" "$DUMP"; then
    echo "guardrail: allocation trace readiness dump missing IR marker: $needle" >&2
    exit 1
  fi
done

if ! rg -F -q 'm6_jit_gc2_readiness.sh' "$ROOT/tools/ci/m6_jit.sh"; then
  echo "guardrail: m6_jit_gc2_readiness.sh is not wired into the M6 aggregate" >&2
  exit 1
fi

echo "M6 JIT GC2 readiness guard passed"
