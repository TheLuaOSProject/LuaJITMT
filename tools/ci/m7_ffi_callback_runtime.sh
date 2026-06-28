#!/bin/sh
# Run the M7 FFI callback runtime guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_callback_runtime
