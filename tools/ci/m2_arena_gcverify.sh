#!/bin/sh
# Run the Lua-defined arena GC metadata verifier path.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m2_arena_gcverify
