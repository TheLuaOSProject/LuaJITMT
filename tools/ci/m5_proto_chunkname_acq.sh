#!/bin/sh
# Run the Lua-defined prototype chunkname acquire-reader guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -RInE -- 'proto_chunkname(str)?[(]' \
    "$ROOT/src" "$ROOT/tests/t-gc2-traverse.c" | \
    grep -vF -- "$ROOT/src/lj_obj.h:" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'published prototype chunkname readers must use proto_chunkname*_acq()' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m5_proto_chunkname_acq
