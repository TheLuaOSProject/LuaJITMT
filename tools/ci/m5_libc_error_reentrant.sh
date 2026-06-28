#!/bin/sh
# Run the Lua-defined M5 reentrant libc error-string guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

exec "$ROOT/tools/ci/lua_test.sh" m5_libc_error_reentrant
