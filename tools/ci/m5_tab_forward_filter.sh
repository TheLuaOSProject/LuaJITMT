#!/bin/sh
# Run the Lua-defined M5 table FORWARD value filtering guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m5_tab_forward_filter
