#!/bin/sh
# M7 FFI guard with Lua suite coverage.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(awk '
  /static LJ_AINLINE CTypeID ctype_(childid|rawid|rawrefid|rawchildid)\(/ { in_fn = 1 }
  in_fn && /ct[[:space:]]*->[[:space:]]*info/ { print FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lj_ctype.h" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType.info reads are forbidden in ctype ID walkers; use ctype_info_acq()' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_ctype_pointer_ids
