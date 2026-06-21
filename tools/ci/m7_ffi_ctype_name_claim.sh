#!/bin/sh
# Compatibility launcher for migrated Lua M7 FFI case.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- '\*dst[[:space:]]*=[[:space:]]*\*src' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lj_cparse.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType stale-slot struct copies are forbidden; use ctype_copy_rel()' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_ctype_name_claim
