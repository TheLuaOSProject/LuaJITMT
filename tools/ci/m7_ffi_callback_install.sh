#!/bin/sh
# Compatibility launcher for migrated Lua M7 FFI case.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- 'cts[[:space:]]*->[[:space:]]*cb[.](mcode|cbid|owner|func|sizeid)|&[[:space:]]*cts[[:space:]]*->[[:space:]]*cb[.](mcode|cbid|owner|func|sizeid)' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/src/lib_ffi.c" \
    "$ROOT/src/lj_ccallback.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CTState callback access is forbidden; use ctype_cb_* helpers' >&2
  exit 1
fi
if hits=$(awk '
  /^static int ffi_callback_set\(/ { in_fn = 1 }
  in_fn && /->[[:space:]]*(info|size)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lib_ffi.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType info/size reads are forbidden in FFI callback install helpers; use ctype_info_acq() or ctype_size_acq()' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_callback_install
