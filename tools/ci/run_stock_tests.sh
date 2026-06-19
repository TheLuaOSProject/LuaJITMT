#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN=${1:-"$ROOT/src/luajit"}
if [ "$#" -gt 0 ]; then
  shift
fi
exec "$ROOT/tools/ci/lua_test.sh" run_stock_tests -- "$BIN" "$@"
