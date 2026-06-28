#!/bin/sh
# Run the M6 JIT GC2 allocation-pacing readiness guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m6_jit_gc2_readiness
