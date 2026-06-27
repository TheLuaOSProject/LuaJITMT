#!/bin/sh
# Run the Lua-defined M3 interpreter-only stock-suite guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m3_interp_stock_joff
