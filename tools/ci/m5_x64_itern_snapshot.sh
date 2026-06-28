#!/bin/sh
# Run the M5 x64 BC_ITERN snapshot guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m5_x64_itern_snapshot
