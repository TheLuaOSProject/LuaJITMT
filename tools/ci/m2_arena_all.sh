#!/bin/sh
# Run the Lua-defined focused M2 arena scaffold test suite.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m2_arena_all
