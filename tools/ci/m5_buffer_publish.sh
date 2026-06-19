#!/bin/sh
# Run the Lua-defined M5 string.buffer publication guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m5_buffer_publish
