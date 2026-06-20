#!/bin/sh
# Run the Lua-defined prototype KGC acquire-reader guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -RInF -- 'proto_kgc(' \
    "$ROOT/src/lib_jit.c" \
    "$ROOT/src/lj_debug.c" \
    "$ROOT/src/lj_dispatch.c" \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/src/lj_meta.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'published prototype KGC readers must use proto_kgc_acq()' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m5_proto_kgc_acq
