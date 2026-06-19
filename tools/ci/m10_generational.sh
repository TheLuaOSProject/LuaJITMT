#!/bin/sh
# Run the Lua-defined M10 generational-mode guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m10_generational
