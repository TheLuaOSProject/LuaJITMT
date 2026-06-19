#!/bin/sh
# Run the Lua-defined M5 internal table sentinel TValue guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m5_itype_sentinel
