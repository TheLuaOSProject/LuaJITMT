#!/bin/sh
# Run the Lua-defined prototype numeric-constant acquire-reader guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -RInE -- 'proto_knumtv[(]|mref[(][^)]*->[[:space:]]*k,[[:space:]]*c?TValue' \
    "$ROOT/src/lib_jit.c" \
    "$ROOT/src/lj_bcwrite.c" \
    "$ROOT/src/lj_meta.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'published prototype numeric constant readers must use proto_knumtv_load_acq()' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m5_proto_knum_acq
