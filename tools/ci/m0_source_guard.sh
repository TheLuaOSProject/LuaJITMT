#!/bin/sh
# Run the Lua-defined M0 test-framework source guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m0_source_guard
