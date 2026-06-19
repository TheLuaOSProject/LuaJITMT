#!/bin/sh
# Run the Lua-defined M6 JIT scaffold gates.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m6_jit
