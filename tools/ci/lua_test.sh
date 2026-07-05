#!/bin/sh
# Canonical launcher for the Lua test suite.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
LOCK_DIR=$ROOT/src/.lj-test-run.lock
LOCK_HELD=0

cleanup_lock() {
  if [ "$LOCK_HELD" = 1 ]; then
    rm -f "$LOCK_DIR/owner"
    rmdir "$LOCK_DIR" 2>/dev/null || true
  fi
}

acquire_lock() {
  if [ "${LJ_TEST_DISABLE_RUN_LOCK:-}" = 1 ] ||
     [ "${LJ_TEST_RUN_LOCK_HELD:-}" = 1 ]; then
    return
  fi

  timeout=${LJ_TEST_RUN_LOCK_TIMEOUT:-900}
  started=$(date +%s)
  announced=0

  while ! mkdir "$LOCK_DIR" 2>/dev/null; do
    now=$(date +%s)
    if [ "$timeout" -ge 0 ] && [ $((now - started)) -ge "$timeout" ]; then
      echo "Lua test runner lock timed out: $LOCK_DIR" >&2
      if [ -f "$LOCK_DIR/owner" ]; then
	echo "owner:" >&2
	cat "$LOCK_DIR/owner" >&2 || true
      fi
      exit 2
    fi
    if [ "$announced" = 0 ]; then
      echo "waiting for Lua test runner lock: $LOCK_DIR" >&2
      announced=1
    fi
    sleep 0.2
  done

  LOCK_HELD=1
  trap cleanup_lock EXIT INT TERM HUP
  {
    printf 'time=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'cmd=%s\n' "$*"
  } >"$LOCK_DIR/owner" 2>/dev/null || true
}

acquire_lock "$@"

if [ -n "${LUA:-}" ]; then
  LUA_BIN=$LUA
elif [ -x "$ROOT/src/luajit" ]; then
  LUA_BIN="$ROOT/src/luajit"
elif [ -x "$ROOT/src/luajit.exe" ]; then
  LUA_BIN="$ROOT/src/luajit.exe"
elif command -v luajit >/dev/null 2>&1; then
  LUA_BIN=luajit
else
  JOBS=${JOBS:-${MAKE_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}}
  make -C "$ROOT/src" -j"$JOBS" >/dev/null
  LUA_BIN="$ROOT/src/luajit"
fi

LJ_TEST_ROOT=$ROOT LJ_TEST_RUN_LOCK_HELD=1 "$LUA_BIN" "$ROOT/tools/test.lua" "$@"
