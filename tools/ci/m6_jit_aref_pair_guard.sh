#!/bin/sh
# Run the M6 JIT shared-array AREF generation-pair guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m6_jit_aref_pair_guard
