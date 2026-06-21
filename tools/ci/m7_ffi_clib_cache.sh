#!/bin/sh
# Compatibility launcher for migrated Lua M7 FFI case.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- '->[[:space:]]*next\b' \
  "$ROOT/src/lj_clib.c" \
  "$ROOT/src/lj_gc.c" \
  "$ROOT/src/lj_gc2.c" \
  "$ROOT/src/lj_crecord.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CLibCacheEntry next-link access is forbidden; use lj_clib_cache_next_* helpers' >&2
  exit 1
fi
if hits=$(awk '
  /^LJLIB_CF\(ffi_clib___index\)/ || /^LJLIB_CF\(ffi_clib___newindex\)/ { in_fn = 1 }
  in_fn && /->[[:space:]]*(info|size)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lib_ffi.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType info/size reads are forbidden in FFI C library extern helpers; use ctype_info_acq() or ctype_size_acq()' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_clib_cache
