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
if hits=$(awk '
  /^static CTypeID cp_struct_name\(/ { in_fn = 1 }
  /^\/\* Determine field alignment/ { in_fn = 0 }
  in_fn && /->[[:space:]]*(info|size)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
' "$ROOT/src/lj_cparse.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw parser struct/enum CType info/size access is forbidden; use ctype_info_* or ctype_size_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- '->[[:space:]]*(info|size|sib|name)([^[:alnum:]_]|$)|setgcrefnull\([^)]*->[[:space:]]*name' \
    "$ROOT/tests/t-ffi-ctype-name-claim.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw name-claim fixture CType access is forbidden; use ctype helper loads/stores' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_ctype_name_claim
