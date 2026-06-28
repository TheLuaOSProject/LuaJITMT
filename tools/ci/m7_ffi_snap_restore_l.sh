#!/bin/sh
# Run the M7 FFI snapshot-restore cdata allocation guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_snap_restore_l
