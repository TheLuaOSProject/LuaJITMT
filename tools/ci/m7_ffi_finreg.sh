#!/bin/sh
# Compatibility launcher for migrated Lua M7 FFI case.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- '(^|[^[:alnum:]_])(gen|ord)[[:space:]]*->[[:space:]]*next' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lj_gc.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw FINREG next-link access is forbidden; use fin_*_next_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- 'ord[[:space:]]*->[[:space:]]*(obj|tab|slot)' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lj_gc.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw FINREG ordered-node payload access is forbidden; use fin_order_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- 'ord[[:space:]]*->[[:space:]]*(retired_next|active)' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lj_gc.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw FINREG ordered-node retire state access is forbidden; use fin_order_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- '(makewhite|markfinalized|lj_gc_arena_markobj|lj_gc2_finreg_cdata_queue|lj_gc2_finalizer_enqueue)[(].*obj2gco[(]cd[)]' \
    "$ROOT/src/lj_cdata.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'cdata sweep/free must not rescue finalizers; FINREG discovery owns finalizer queueing' >&2
  exit 1
fi
"$ROOT/tools/ci/lua_test.sh" m7_ffi_finreg
cc -std=gnu99 -O2 -Wall -Wextra -Werror -mcx16 -I"$ROOT/src" \
  "$ROOT/tests/t-ffi-finreg-free-invariant.c" "$ROOT/src/libluajit.a" \
  -lm -ldl -pthread -o /tmp/lj_t-ffi-finreg-free-invariant
/tmp/lj_t-ffi-finreg-free-invariant
