#!/bin/sh
# Run the Lua-defined arena bitmap scaffold test.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m2_arena_bitmap
