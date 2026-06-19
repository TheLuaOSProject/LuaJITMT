#!/bin/sh
# Run the Lua-defined arena allocation-color GC phase test.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m2_arena_gcphase
