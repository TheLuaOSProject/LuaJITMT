#!/bin/sh
# Run the Lua-defined M3 GC2 worker scheduler guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m3_gc2_worker_scheduler
