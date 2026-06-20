#!/bin/sh
# Compatibility launcher for migrated Lua M7 FFI case.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- '(^|[^[:alnum:]_])(gen|ord)[[:space:]]*->[[:space:]]*next' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lj_gc.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw FINREG next-link access is forbidden; use fin_*_next_* helpers' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_finreg
