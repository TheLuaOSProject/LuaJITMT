#!/bin/sh
# Run the Lua-defined M3 VM safepoint guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m3_vm_safepoint
