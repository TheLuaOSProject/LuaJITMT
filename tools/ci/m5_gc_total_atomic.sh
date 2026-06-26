#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT"

for helper in lj_gc_total_load lj_gc_total_store lj_gc_total_add \
  lj_gc_total_sub lj_gc_total_adjust; do
  if ! grep -q "static LJ_AINLINE .* ${helper}" src/lj_gc.h; then
    printf '%s\n' "${helper} helper is required for GC total accounting" >&2
    exit 1
  fi
done

if hits=$(grep -nE '__atomic_fetch_(add|sub)' src/lj_gc.h || true); \
    [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'GC total accounting must use lj_atomic.h fetch helpers' >&2
  exit 1
fi

if hits=$(grep -RInE -- 'g->gc\.total|G\(L\)->gc\.total' \
    src/lj_*.c src/lib_*.c src/lj_*.h 2>/dev/null | \
    grep -Ev '^(src/lj_gc\.h:|src/lj_obj\.h:|src/lj_asm_[^:]+\.h:)' || true); \
    [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'C-side GC total access must use lj_gc_total_* helpers' >&2
  exit 1
fi

if ! grep -q "lj_gc_should_step_vm" src/lj_gc.h ||
   ! grep -q "lj_gc_should_step_vm" src/vm_x64.dasc; then
  printf '%s\n' 'x64 VM allocation checks must call lj_gc_should_step_vm' >&2
  exit 1
fi

if hits=$(grep -nE 'GL:[^ ]+->(gc\.(total|threshold)|gc2\.(alloc_since_trigger|hard_bytes))|x64_vm_gc(_total|_threshold|2_alloc_since|2_hard_bytes)_acq' \
    src/vm_x64.dasc || true); \
    [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'x64 VM allocation checks must keep GC pacing reads in C helpers' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m5_gc_total_atomic
