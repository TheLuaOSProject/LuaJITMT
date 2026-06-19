#!/bin/sh
# Run the Lua-defined x64 JIT HREF table node/hmask load-order guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m5_jit_href_node_order
