#!/bin/sh
# Run the Lua-defined M5 x64 table next snapshot guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m5_x64_table_next_snapshot
