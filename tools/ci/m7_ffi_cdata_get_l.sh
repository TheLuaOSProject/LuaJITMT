#!/bin/sh
# Compatibility launcher for migrated Lua M7 FFI case.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(awk '
  /^(void LJ_FASTCALL lj_cdata_free|CType \*lj_cdata_index_l|static void cdata_getconst|int lj_cdata_get_l|void lj_cdata_set_l)\(/ { in_fn = 1 }
  in_fn && /->[[:space:]]*(info|size)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lj_cdata.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType info/size reads are forbidden in cdata core helpers; use ctype_info_acq() or ctype_size_acq()' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_cdata_get_l
