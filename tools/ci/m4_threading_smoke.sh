#!/bin/sh
# Run the Lua-defined M4 concurrent pure-compute threading smoke test.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m4_threading_smoke
