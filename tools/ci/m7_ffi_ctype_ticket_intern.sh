#!/bin/sh
# Compatibility launcher for migrated Lua M7 FFI case.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- 'cts[[:space:]]*->[[:space:]]*(tabh|top)|&[[:space:]]*cts[[:space:]]*->[[:space:]]*(tabh|top)' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CTState ctype table/top access is forbidden; use ctype_tabh_* and ctype_top_* helpers' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_ctype_ticket_intern
