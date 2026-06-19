#!/bin/sh
# Run the Lua-defined M5 table hash-node TValue snapshot guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m5_tab_slot_snapshot
