#!/bin/sh
# Run the M6 JIT XBAR/XPOLL alias guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m6_jit_xbar_xpoll
