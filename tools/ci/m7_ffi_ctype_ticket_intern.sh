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
if hits=$(grep -nE -- 'ct[[:space:]]*->[[:space:]]*(info|size)[[:space:]]*=[^=]' \
    "$ROOT/src/lj_ctype.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw core CType payload stores are forbidden; use ctype_info_rel() or ctype_size_rel()' >&2
  exit 1
fi
if hits=$(grep -nE -- 'ct[[:space:]]*->[[:space:]]*info[[:space:]]*=[[:space:]]*CTINFO\(CT_ATTRIB,[[:space:]]*CTATTRIB\(CTA_BAD\)\)|ct[[:space:]]*->[[:space:]]*size[[:space:]]*=[[:space:]]*0' \
    "$ROOT/src/lj_cparse.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw parser CType abandon payload stores are forbidden; use ctype_info_rel() or ctype_size_rel()' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_ctype_ticket_intern
