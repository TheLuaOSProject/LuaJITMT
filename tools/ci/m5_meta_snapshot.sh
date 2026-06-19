#!/bin/sh
# Run the Lua-defined metamethod lookup snapshot guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m5_meta_snapshot
