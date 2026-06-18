#!/bin/sh
# Guard the M10 public generational-mode control surface.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
OUT=${TMPDIR:-/tmp}/lj_t-gc2-alloc-account-m10

make -C "$ROOT/src" >/dev/null

for needle in \
  'LUA_GCGENERATIONAL' \
  'LUA_GCINCREMENTAL' \
  'uint32_t generational' \
  'uint32_t cycle_minor_requested' \
  'uint32_t force_major' \
  'uint64_t major_cycle_starts' \
  'uint64_t minor_cycle_requests' \
  'la_store32_rlx(&g->gc2.generational, 0)' \
  'la_store32_rel(&g->gc2.generational, 1)' \
  'la_store32_rel(&g->gc2.generational, 0)' \
  'lj_gc2_force_major(global_State *g)' \
  'gc_stats_setint(L, t, "generational"' \
  'minor_cycle_requests' \
  'collectgarbage("generational")' \
  'collectgarbage("incremental")'
do
  if ! rg -F -q "$needle" "$ROOT/src/lua.h" "$ROOT/src/lj_obj.h" \
      "$ROOT/src/lj_gc2.c" "$ROOT/src/lj_api.c" "$ROOT/src/lib_base.c" \
      "$ROOT/tests/t-gc-generational-mode.lua" "$ROOT/tests/t-gc-stats.lua" \
      "$ROOT/tests/t-gc2-alloc-account.c"; then
    echo "guardrail: missing M10 generational marker: $needle" >&2
    exit 1
  fi
done

"$ROOT/src/luajit" -joff "$ROOT/tests/t-gc-generational-mode.lua"
"$ROOT/src/luajit" "$ROOT/tests/t-gc-generational-mode.lua"

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-gc2-alloc-account.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
timeout 20s "$OUT"

echo "M10 generational mode guard passed"
