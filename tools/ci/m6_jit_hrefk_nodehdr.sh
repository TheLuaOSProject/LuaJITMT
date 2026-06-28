#!/bin/sh
# Run the M6 JIT HREFK node-header guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m6_jit_hrefk_nodehdr
