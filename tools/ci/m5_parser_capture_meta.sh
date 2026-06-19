#!/bin/sh
# Run the Lua-defined parser captured-local metadata guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m5_parser_capture_meta
