#!/bin/sh
# Compatibility launcher for migrated Lua M7 FFI case.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- 'cts[[:space:]]*->[[:space:]]*(miscmap|metamap|sizemeta)|&[[:space:]]*cts[[:space:]]*->[[:space:]]*(miscmap|metamap|sizemeta)' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lib_ffi.c" \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CTState metatype/miscmap access is forbidden; use ctype_* helpers' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_metatype
