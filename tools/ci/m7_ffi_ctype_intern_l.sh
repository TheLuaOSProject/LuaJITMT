#!/bin/sh
# Run the M7 FFI ctype allocation/interning guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_ctype_intern_l
