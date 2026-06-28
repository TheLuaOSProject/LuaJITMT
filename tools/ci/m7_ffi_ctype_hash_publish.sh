#!/bin/sh
# M7 FFI guard with Lua suite coverage.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if grep -nE -- '(^|[^[:alnum:]_])(ct|dst|basect)[[:space:]]*->[[:space:]]*next[[:space:]]*([=;)]|$)' \
  "$ROOT/src/lj_ctype.c" \
  "$ROOT/src/lib_ffi.c" \
  "$ROOT/tests/t-ffi-ctype-name-claim.c" \
  "$ROOT/tests/t-ffi-ctype-ticket-intern.c"
then
  echo "raw CType.next hash-chain access escaped helper" >&2
  exit 1
fi
if hits=$(grep -nE -- 'cts[[:space:]]*->[[:space:]]*hash|&[[:space:]]*cts[[:space:]]*->[[:space:]]*hash' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lib_ffi.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CTState ctype hash head access is forbidden; use ctype_hash_head_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- 'ctype_isabandoned\(ct->[[:space:]]*info\)|ct->[[:space:]]*info[[:space:]]*==[[:space:]]*info|ct->[[:space:]]*size[[:space:]]*==[[:space:]]*size|ctype_type\(ct->[[:space:]]*info\)|ct_hashtype\(ct->[[:space:]]*info,[[:space:]]*ct->[[:space:]]*size\)' \
    "$ROOT/src/lj_ctype.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType hash-walker payload reads are forbidden; use ctype_info_acq() or ctype_size_acq()' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_ctype_hash_publish
