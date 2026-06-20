#!/bin/sh
# Run the Lua-defined M5 x64 getmetatable node-order guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- '(->[[:space:]]*(metatable|env)([^[:alnum:]_]|$)|GL_OFS[(]gcroot|GCROOT_)' \
    "$ROOT/src/vm_x64.dasc" | \
    grep -vE -- 'x64_vm_(gcref|gcroot|basemt)_' || true); \
    [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw x64 VM GCRef edge access is forbidden; use x64_vm_gcref/gcroot helpers' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m5_x64_getmetatable_node_order
