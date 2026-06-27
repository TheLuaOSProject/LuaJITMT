#!/bin/sh
# Run the Lua-defined M5 direct registry root publication guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- 'lj_gc_barrierroot[[:space:]]*[(]|LJ_FUNC .*lj_gc_barrierroot[[:space:]]*[(]' \
    "$ROOT"/src/*.c "$ROOT/src/lj_gc.h" || true); then
  if [ -n "$hits" ]; then
    printf '%s\n' "$hits" >&2
    printf '%s\n' 'legacy-named root barrier is forbidden; use lj_gc_pubroot for root publication' >&2
    exit 1
  fi
fi
if ! grep -qE 'LJ_FUNC void lj_gc_pubroot[[:space:]]*[(]' \
    "$ROOT/src/lj_gc.h"; then
  printf '%s\n' 'lj_gc_pubroot declaration is required for root publication' >&2
  exit 1
fi
if ! grep -qE '^void lj_gc_pubroot[[:space:]]*[(]' \
    "$ROOT/src/lj_gc.c"; then
  printf '%s\n' 'lj_gc_pubroot definition is required for root publication' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m5_registry_root
