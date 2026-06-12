#!/bin/sh
# Run focused M4 Lua-visible threading API smoke tests.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" "$ROOT/tests/t-threading-api.lua"

echo "M4 threading API tests passed"
