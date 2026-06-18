#!/bin/sh
# Guard the M10 public generational-mode control surface.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

make -C "$ROOT/src" >/dev/null

for needle in \
  'LUA_GCGENERATIONAL' \
  'LUA_GCINCREMENTAL' \
  'uint32_t generational' \
  'la_store32_rlx(&g->gc2.generational, 0)' \
  'la_store32_rel(&g->gc2.generational, 1)' \
  'la_store32_rel(&g->gc2.generational, 0)' \
  'gc_stats_setint(L, t, "generational"' \
  'collectgarbage("generational")' \
  'collectgarbage("incremental")'
do
  if ! rg -F -q "$needle" "$ROOT/src/lua.h" "$ROOT/src/lj_obj.h" \
      "$ROOT/src/lj_gc2.c" "$ROOT/src/lj_api.c" "$ROOT/src/lib_base.c" \
      "$ROOT/tests/t-gc-generational-mode.lua" "$ROOT/tests/t-gc-stats.lua"; then
    echo "guardrail: missing M10 generational marker: $needle" >&2
    exit 1
  fi
done

"$ROOT/src/luajit" -joff "$ROOT/tests/t-gc-generational-mode.lua"
"$ROOT/src/luajit" "$ROOT/tests/t-gc-generational-mode.lua"

echo "M10 generational mode guard passed"
