#!/bin/sh
# Run the M5 x64 TGETS/TSETS node-order guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m5_x64_tgets_node_order
