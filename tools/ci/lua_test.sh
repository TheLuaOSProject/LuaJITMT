#!/bin/sh
# Canonical launcher for the Lua test suite.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

if [ -n "${LUA:-}" ]; then
  LUA_BIN=$LUA
elif [ -x "$ROOT/src/luajit" ]; then
  LUA_BIN="$ROOT/src/luajit"
elif command -v luajit >/dev/null 2>&1; then
  LUA_BIN=luajit
elif command -v lua >/dev/null 2>&1; then
  LUA_BIN=lua
else
  JOBS=${JOBS:-${MAKE_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}}
  make -C "$ROOT/src" -j"$JOBS" >/dev/null
  LUA_BIN="$ROOT/src/luajit"
fi

LJ_TEST_ROOT=$ROOT exec "$LUA_BIN" "$ROOT/tools/test.lua" "$@"
