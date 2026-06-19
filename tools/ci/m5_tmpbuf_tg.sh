#!/bin/sh
# Run the Lua-defined M5 per-TG temporary string buffer guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m5_tmpbuf_tg
