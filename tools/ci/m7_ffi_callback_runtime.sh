#!/bin/sh
# Compatibility launcher for migrated Lua M7 FFI case.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- 'cts[[:space:]]*->[[:space:]]*(cbblack|sizecbblack|cbblack_all)|&[[:space:]]*cts[[:space:]]*->[[:space:]]*(cbblack|sizecbblack|cbblack_all)' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CTState callback blacklist access is forbidden; use ctype_cbblack_* helpers' >&2
  exit 1
fi
if hits=$(awk '
  /^static void callback_conv_args\(/ || /^static void callback_conv_result\(/ { in_fn = 1 }
  in_fn && /->[[:space:]]*(info|size|sib)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lj_ccallback.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType info/size/sib reads are forbidden in callback runtime conversion helpers; use ctype_*_acq() helpers' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_callback_runtime
