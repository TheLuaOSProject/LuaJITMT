#!/bin/sh
# Run the M6 JIT C-closure upvalue mutation flush guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m6_jit_cclosure_upvalue_flush
