#!/bin/sh
# Guard x64 closed-upvalue store publication routing.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

for file in \
  "$ROOT/src/vm_x64.dasc" \
  "$ROOT/src/lj_asm_x86.h" \
  "$ROOT/src/lj_ircall.h"
do
  hits=$(rg -n 'lj_gc_barrieruv|IRCALL_lj_gc_barrieruv' "$file" || true)
  if [ -n "$hits" ]; then
    echo "guardrail: x64/JIT upvalue stores must use lj_gc_pubuv:" >&2
    echo "$hits" >&2
    exit 1
  fi
done

if ! awk '
  /void LJ_FASTCALL lj_gc_pubuv/ { infn = 1; seen = 1 }
  infn && /lj_gc2_barrier_uv\(g, tv\)/ { gc2 = 1 }
  infn && /gc_mark\(g, gcV\(tv\)\)/ { legacy = 1 }
  infn && /TV2MARKED\(tv\).*curwhite\(g\)/ { white = 1 }
  infn && /^}/ { exit(seen && gc2 && legacy && white ? 0 : 1) }
  END { if (!seen || !gc2 || !legacy || !white) exit 1 }
' "$ROOT/src/lj_gc.c"; then
  echo "guardrail: lj_gc_pubuv must preserve GC2 and legacy upvalue publication behavior" >&2
  exit 1
fi

for needle in \
  'call extern lj_gc_pubuv' \
  'IRCALL_lj_gc_pubuv' \
  'lj_gc_pubuv,'
do
  if ! rg -F -q "$needle" "$ROOT/src/vm_x64.dasc" "$ROOT/src/lj_asm_x86.h" "$ROOT/src/lj_ircall.h"; then
    echo "guardrail: missing x64/JIT upvalue publication marker: $needle" >&2
    exit 1
  fi
done

echo "M5 x64 upvalue publication guard passed"
