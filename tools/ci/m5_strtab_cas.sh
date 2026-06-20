#!/bin/sh
# Run the Lua-defined M5 string table CAS publication guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- '->[[:space:]]*retired_next' \
    "$ROOT/src/lj_str.c" \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/tests/t-strtab-cas.c" \
    "$ROOT/tests/t-strtab-prep.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw StrTabHdr retired_next access is forbidden; use lj_str_retired_next_* helpers' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m5_strtab_cas
