#!/bin/sh
# Run the Lua-defined M10 generational-mode guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

fields='generational|force_major|cycle_minor_requested|cycle_sweep_minor|cycle_roots_minor|minor_sweep_enabled|minor_roots_enabled|minor_survival_pct|minor_survival_threshold_pct'
if hits=$(grep -nE -- "->[[:space:]]*gc2[.](${fields})([^[:alnum:]_]|$)|gc2[[:space:]]*->[[:space:]]*(${fields})([^[:alnum:]_]|$)" \
  "$ROOT/src/lj_api.c" \
  "$ROOT/src/lj_gc.c" \
  "$ROOT/src/lj_gc2.c" \
  "$ROOT/src/lj_tg.c" \
  "$ROOT/src/lib_base.c" || true); then
  if [ -n "$hits" ]; then
    printf '%s\n' \
      "raw GC2 generational/minor gate state access is forbidden; use gc2_* mode/gate helpers" \
      "$hits" >&2
    exit 1
  fi
fi

exec "$ROOT/tools/ci/lua_test.sh" m10_generational
