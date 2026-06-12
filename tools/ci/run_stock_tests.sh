#!/bin/sh
# Run the vendored LuaJIT stock cleanup suite with a selected LuaJIT binary.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN=${1:-"$ROOT/src/luajit"}
if [ "$#" -gt 0 ]; then
  shift
fi
case "$BIN" in
  /*) ;;
  *) BIN="$ROOT/$BIN" ;;
esac

if [ ! -x "$BIN" ]; then
  echo "stock tests: LuaJIT binary is not executable: $BIN" >&2
  exit 2
fi

cd "$ROOT/tests/stock/test"
exec "$BIN" test.lua "$@"
