#!/bin/sh
# Run the Lua-defined M3 safepoint handshake guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m3_safepoint_handshake
