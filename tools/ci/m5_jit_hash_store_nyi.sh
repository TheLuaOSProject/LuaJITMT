#!/bin/sh
# Run the Lua-defined helper-backed table store guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m5_jit_hash_store_nyi
