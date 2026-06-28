#!/bin/sh
# Run the M7 FFI duplicate cdef stack-growth guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_cdef_dup_stack
