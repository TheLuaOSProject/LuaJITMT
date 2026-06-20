#!/bin/sh
# Run the Lua-defined M3 GC2 scaffold aggregate.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

check_raw_finreg_udata_next() {
  label=$1
  file=$2
  start=$3
  if hits=$(sed -n "/$start/,/^}/p" "$file" | \
      grep -nE -- 'node[[:space:]]*->[[:space:]]*next' || true); \
      [ -n "$hits" ]; then
    printf '%s\n' "$label" >&2
    printf '%s\n' "$hits" >&2
    printf '%s\n' 'raw GC2 FINREG userdata next-link access is forbidden; use gc2_finreg_udata_next_* helpers' >&2
    exit 1
  fi
}

check_raw_finreg_udata_next "src/lj_gc.c:gc_separateudata_registered" \
  "$ROOT/src/lj_gc.c" "static size_t gc_separateudata_registered"
check_raw_finreg_udata_next "src/lj_gc2.c:lj_gc2_fini" \
  "$ROOT/src/lj_gc2.c" "void lj_gc2_fini"
check_raw_finreg_udata_next "src/lj_gc2.c:lj_gc2_finreg_udata_register" \
  "$ROOT/src/lj_gc2.c" "void lj_gc2_finreg_udata_register"
check_raw_finreg_udata_next "src/lj_gc2.c:lj_gc2_finreg_udata_forget" \
  "$ROOT/src/lj_gc2.c" "void lj_gc2_finreg_udata_forget"

exec "$ROOT/tools/ci/lua_test.sh" m3_gc2_scaffold
