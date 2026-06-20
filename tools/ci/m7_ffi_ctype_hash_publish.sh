#!/bin/sh
# Compatibility launcher for migrated Lua M7 FFI case.
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

exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_ctype_hash_publish
