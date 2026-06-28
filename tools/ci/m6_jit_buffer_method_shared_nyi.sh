#!/bin/sh
# Run the M6 JIT string.buffer shared-method NYI guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m6_jit_buffer_method_shared_nyi
