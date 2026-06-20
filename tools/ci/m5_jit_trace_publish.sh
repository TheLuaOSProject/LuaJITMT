#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -RInF -- '->trace[' "$ROOT/src" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw JIT trace-slot indexing is forbidden; use traceslot_* helpers' >&2
  exit 1
fi
if hits=$(grep -nF -- 'J_OFS(trace)' "$ROOT/src/vm_x64.dasc" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'x64 VM trace-slot loads must use J->tracev, not J->trace' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m5_jit_trace_publish
