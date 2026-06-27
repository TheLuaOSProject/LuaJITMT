#!/bin/sh
# Run the Lua-defined M5 table hash-vector retirement guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- '(^|[^[:alnum:]_])(ret|aret)[[:space:]]*->[[:space:]]*(next|node|hmask|array|acap|retire_epoch|armed)' \
    "$ROOT/src/lj_tab.c" \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/tests/t-tab-retire.c" \
    "$ROOT/tests/t-tab-array-publish.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw table retire record access is forbidden; use lj_tab_*_retired_* helpers' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m5_tab_retire
