#!/bin/sh
# Run the Lua-defined M5 JIT table field FLOAD mutability guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m5_jit_table_fload_mutable
