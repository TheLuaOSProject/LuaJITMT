#!/bin/sh
# Run the M6 JIT threaded string.format tmpbuf guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m6_jit_tmpbuf_thread_format
