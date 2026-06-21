#!/bin/sh
# Compatibility launcher for migrated Lua M7 FFI case.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -RInE -- '->[[:space:]]*retired_next' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/tests/t-ffi-ctype-tab-retire.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CTypeTab retired_next access is forbidden; use ctype_tab_retired_next_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- '(ctret|ret)[[:space:]]*->[[:space:]]*retired_next' \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC CTypeTab retired_next traversal is forbidden; use ctype_tab_retired_next_acq' >&2
  exit 1
fi
if hits=$(grep -nE -- 'cts[[:space:]]*->[[:space:]]*retiredtab|&[[:space:]]*cts[[:space:]]*->[[:space:]]*retiredtab' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CTState retired CTypeTab head access is forbidden; use ctype_retiredtab_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- '->[[:space:]]*retire_epoch|&[[:space:]]*[a-z]+[[:space:]]*->[[:space:]]*retire_epoch' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/tests/t-ffi-ctype-tab-retire.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CTypeTab retire_epoch access is forbidden; use ctype_tab_retire_epoch_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- '->[[:space:]]*sizetab|&[[:space:]]*[a-z]+[[:space:]]*->[[:space:]]*sizetab' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lib_ffi.c" \
    "$ROOT/tests/t-ffi-ctype-tab-retire.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CTypeTab sizetab access is forbidden; use ctype_tab_sizetab_* helpers' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_ctype_tab_retire
