#!/bin/sh
# Run the current M7 FFI concurrency gates.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

"$ROOT/tools/ci/m7_ffi_cdef_token.sh"

echo "M7 FFI gates passed"
