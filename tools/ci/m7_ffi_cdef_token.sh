#!/bin/sh
# Compatibility launcher for migrated Lua M7 FFI case.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- 'cts[[:space:]]*->[[:space:]]*parse_token|&[[:space:]]*cts[[:space:]]*->[[:space:]]*parse_token|[a-z]+->[[:space:]]*cts[[:space:]]*->[[:space:]]*parse_token|&[a-z]+->[[:space:]]*cts[[:space:]]*->[[:space:]]*parse_token' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lib_ffi.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CTState parse-token access is forbidden; use ctype_parse_token_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- 'la_cpu_pause[[:space:]]*[(]' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lib_ffi.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'FFI ctype/parser waits must park or sleep through native helpers, not spin on la_cpu_pause()' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_cdef_token
