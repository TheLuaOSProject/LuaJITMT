#!/bin/sh
# Run the Lua-defined M9/M10 GC telemetry and generational gates.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m9_m10_gc
