#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -RInF -- '->trace[' "$ROOT/src" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw JIT trace-slot indexing is forbidden; use traceslot_* helpers' >&2
  exit 1
fi
if hits=$(grep -RInE -- 'GCRef[[:space:]]+\*trace[[:space:]]*;|J->[[:space:]]*trace([^[:alnum:]_]|$)' "$ROOT/src" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'the J->trace slot mirror is forbidden; use J->tracev' >&2
  exit 1
fi
if hits=$(grep -nF -- 'J_OFS(trace)' "$ROOT/src/vm_x64.dasc" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'x64 VM trace-slot loads must use J->tracev, not J->trace' >&2
  exit 1
fi
if hits=$(grep -nE -- '(^|[^[:alnum:]_])(ret|tail|mcret)[[:space:]]*->[[:space:]]*next' \
    "$ROOT/src/lj_mcode.c" \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/tests/t-jit-mcode-retire.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw MCodeRetire next-link access is forbidden; use mcode_retired_next_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- '->[[:space:]]*retired_next' \
    "$ROOT/src/lj_trace.c" \
    "$ROOT/tests/t-jit-trace-retire.c" \
    "$ROOT/tests/t-jit-tracevec.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw JIT retired trace next-link access is forbidden; use trace*_retired_next_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- 'lj_gc_barriertrace[[:space:]]*[(]|LJ_FUNC .*lj_gc_barriertrace[[:space:]]*[(]' \
    "$ROOT"/src/*.c "$ROOT/src/lj_gc.h" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'legacy-named trace barrier is forbidden; use lj_gc_pubtrace for trace publication' >&2
  exit 1
fi
if ! grep -qE 'LJ_FUNC void lj_gc_pubtrace[[:space:]]*[(]' \
    "$ROOT/src/lj_gc.h"; then
  printf '%s\n' 'lj_gc_pubtrace declaration is required for trace publication' >&2
  exit 1
fi
if ! grep -qE '^void lj_gc_pubtrace[[:space:]]*[(]' \
    "$ROOT/src/lj_gc.c"; then
  printf '%s\n' 'lj_gc_pubtrace definition is required for trace publication' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m5_jit_trace_publish
