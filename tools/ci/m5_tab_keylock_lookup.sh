#!/bin/sh
# Run the Lua-defined M5 table KEYLOCK lookup filtering guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m5_tab_keylock_lookup
