#!/bin/sh
# Run the Lua-defined M9 benchmark regression accounting guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m9_bench_regression
