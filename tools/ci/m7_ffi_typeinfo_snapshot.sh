#!/bin/sh
# Compatibility launcher for migrated Lua M7 FFI case.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

if grep -nF 'ok && ctype_isstruct(ctype_get(cts, cid)->info)' \
  "$ROOT/src/lj_cdata.c"
then
  printf '%s\n' 'cdata pointer auto-deref must trust lj_ctype_ptrstruct_snapshot() outside the parser lock' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_typeinfo_snapshot
