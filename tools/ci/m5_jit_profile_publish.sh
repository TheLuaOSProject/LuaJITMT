#!/bin/sh
# Run the Lua-defined M5 jit.profile publication guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m5_jit_profile_publish
