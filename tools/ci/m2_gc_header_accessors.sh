#!/bin/sh
# Run the Lua-defined C-side GC header accessor guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m2_gc_header_accessors
