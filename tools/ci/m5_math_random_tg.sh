#!/bin/sh
# Run the Lua-defined M5 per-TG math.random regression test.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m5_math_random_tg
