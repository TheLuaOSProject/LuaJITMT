#!/bin/sh
# Run the Lua-defined focused M4 public C threading API test.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m4_threading_capi
