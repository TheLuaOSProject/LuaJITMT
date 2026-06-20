#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- '(^|[^[:alnum:]_])(ret|tail|mcret)[[:space:]]*->[[:space:]]*next' \
    "$ROOT/src/lj_mcode.c" \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/tests/t-jit-mcode-retire.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw MCodeRetire next-link access is forbidden; use mcode_retired_next_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- '\(\(MCLink \*\)[^)]*\)->[[:space:]]*next|rwlink[[:space:]]*->[[:space:]]*next' \
    "$ROOT/src/lj_mcode.c" \
    "$ROOT/src/lj_mcode.h" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw MCLink next-link access is forbidden; use mcode_area_next_* helpers' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m6_jit_mcode_publish
