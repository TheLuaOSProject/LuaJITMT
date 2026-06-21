#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT"

for helper in lj_gc2_alloc_since_load lj_gc2_alloc_since_store \
  lj_gc2_alloc_since_add lj_gc2_alloc_since_xchg lj_gc2_cycle_alloc_load \
  lj_gc2_cycle_alloc_store lj_gc2_trigger_load lj_gc2_trigger_store \
  lj_gc2_hard_load lj_gc2_hard_store lj_gc2_hard_limit_reached; do
  if ! grep -q "static LJ_AINLINE .* ${helper}" src/lj_gc.h; then
    printf '%s\n' "${helper} helper is required for GC2 pacing counters" >&2
    exit 1
  fi
done

for helper in gc2_gcpause_pct_acq gc2_gcpause_pct_store_rlx \
  gc2_gcpause_pct_rel; do
  if ! grep -q "static LJ_AINLINE .* ${helper}" src/lj_obj.h; then
    printf '%s\n' "${helper} helper is required for GC2 pause pacing" >&2
    exit 1
  fi
done

if hits=$(grep -RInE -- \
    '(g->gc2|gc2->)\.(alloc_since_trigger|cycle_alloc_bytes|trigger_bytes|hard_bytes|gcpause_pct)' \
    src/lj_*.c src/lib_*.c src/lj_*.h 2>/dev/null | \
    grep -Ev '^(src/lj_gc\.h:|src/lj_obj\.h:|src/lj_asm_[^:]+\.h:)' || true); \
    [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'C-side GC2 pacing access must use helper APIs' >&2
  exit 1
fi

for helper in x64_vm_gc2_alloc_since_acq x64_vm_gc2_hard_bytes_acq; do
  if ! grep -q "^|\\.macro ${helper}" src/vm_x64.dasc; then
    printf '%s\n' "${helper} macro is required for x64 VM GC2 checks" >&2
    exit 1
  fi
done

if hits=$(grep -nE 'GL:[^ ]+->gc2\.(alloc_since_trigger|hard_bytes)' \
    src/vm_x64.dasc | \
    grep -Ev 'x64_vm_gc2_(alloc_since|hard_bytes)_acq' || true); \
    [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'x64 VM GC2 allocation checks must use x64_vm_gc2_* helpers' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m5_gc2_pacing_atomic
