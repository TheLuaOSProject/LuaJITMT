#!/bin/sh
# Guard GC2 allocation-pacing readiness while legacy JIT GCSTEP is still live.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
DUMP=${TMPDIR:-/tmp}/lj_t-jit-gc2-readiness.dump
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
OUT=${TMPDIR:-/tmp}/lj_t-gc2-jit-hard-check

make -C "$ROOT/src" >/dev/null

for needle in \
  'uint64_t jit_hard_checks' \
  'static void gc2_maybe_trigger_cycle(global_State *g)' \
  'lj_gc_threshold_load(g) == LJ_MAX_MEM' \
  'lj_gc_threshold_store(g, g->gc.total)' \
  'lj_gc2_assist(global_State *g, TGState *tg)' \
  'lj_gc2_assist(g, L2TG(L));  /* 05 section 5.11 trace-side assist bridge. */' \
  'legacy_step = g->gc.total >= lj_gc_threshold_load(g)' \
  'la_add64_rlx(&g->gc2.jit_hard_checks' \
  'la_cas32(&g->gc2.assist_active' \
  'gc2_drain_active_ssb_to_grey(global_State *g, TGState *tg' \
  'gc2_drain_published_ssb_to_grey(global_State *g' \
  'emit_getgl(as, tmp, gc2.alloc_since_trigger)' \
  'emit_opgl(as, XO_ARITH(XOg_CMP), tmp|REX_GC64, gc2.hard_bytes)' \
  'checkmclim(as);  /* M6: split trace-head GC check after snapshot prep. */' \
  'checkmclim(as);  /* M6: start long GC check sequence on a fresh red zone. */' \
  'checkmclim(as);  /* M6: split long GC check sequence for assert red zone. */' \
  'checkmclim(as);  /* M6: split GC2-hard and legacy GC threshold tests. */' \
  'lj_gc_step_jit' \
  'IR_GCSTEP' \
  'asm_gc_check'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_gc2.c" "$ROOT/src/lj_gc2.h" \
      "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc.h" "$ROOT/src/lj_obj.h" \
      "$ROOT/src/lj_ir.h" \
      "$ROOT/src/lj_ircall.h" "$ROOT/src/lj_snap.c" \
      "$ROOT/src/lj_asm.c" "$ROOT/src/lj_asm_x86.h"; then
    echo "guardrail: missing GC2/JIT pacing readiness marker: $needle" >&2
    exit 1
  fi
done

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-gc2-jit-hard-check.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
timeout 20s "$OUT"

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
