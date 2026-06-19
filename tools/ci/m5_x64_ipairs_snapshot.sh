#!/bin/sh
# Run the Lua-defined M5 x64 ipairs snapshot guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m5_x64_ipairs_snapshot
