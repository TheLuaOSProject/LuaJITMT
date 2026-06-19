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
  'uint32_t cycle_sweep_minor' \
  'uint32_t minor_sweep_enabled;  /* Public gate for minor sweep identity. */' \
  'uint32_t cycle_roots_minor' \
  'uint32_t minor_roots_enabled;  /* Public gate for minor root selection. */' \
  'uint32_t force_major' \
  'uint64_t major_cycle_starts' \
  'uint64_t minor_cycle_requests' \
  'uint64_t minor_cycle_starts' \
  'uint64_t minor_sweep_deferred' \
  'uint64_t minor_sweep_arenas' \
  'uint64_t minor_roots_deferred' \
  'uint64_t major_root_scans' \
  'uint64_t minor_root_scans' \
  'uint64_t minor_survival_base_live' \
  'uint64_t minor_survival_bytes' \
  'uint32_t minor_survival_pct' \
  'uint32_t minor_survival_threshold_pct' \
  'uint64_t minor_survival_major_requests' \
  'uint64_t cycle_alloc_bytes' \
  'LJ_GC2_MINOR_SURVIVAL_MAJOR_PCT' \
  'la_store32_rlx(&g->gc2.generational, 0)' \
  'uint64_t remembered_barriers' \
  'uint64_t remembered_pushed' \
  'uint64_t remembered_overflows' \
  'uint64_t remembered_filtered' \
  'uint64_t remembered_drained' \
  'lj_gc2_set_generational(global_State *g, int enabled)' \
  'la_store32_rel(&g->gc2.generational, want)' \
  'tg->mark_active = la_load32_acq(&g->gc2.generational) != 0' \
  'lj_gc2_force_major(global_State *g)' \
  'lj_gc2_update_minor_survival_policy(global_State *g, uint64_t live)' \
  'la_store64_rel(&g->gc2.cycle_alloc_bytes' \
  'la_load64_acq(&g->gc2.minor_survival_base_live)' \
  'la_add64_rlx(&g->gc2.minor_survival_major_requests' \
  'if (roots_minor)' \
  'gc2_update_public_minor_gates(global_State *g)' \
  'la_store32_rel(&g->gc2.minor_sweep_enabled, enabled)' \
  'la_store32_rel(&g->gc2.minor_roots_enabled, enabled)' \
  'gc2_remember_obj(global_State *g, GCobj *o)' \
  'gc2_remember_pair(global_State *g, GCobj *parent, GCobj *child)' \
  'lj_gc2_barrier_obj_pair(lua_State *L, GCobj *parent, GCobj *child)' \
  'gc2_flush_and_drain_ssb(global_State *g)' \
  'lj_meta_tsettv_pair(lua_State *L, cTValue *o, cTValue *k' \
  'lj_gc2_scan_minor_roots(global_State *g, lua_State *L)' \
  'lj_gc2_scan_cycle_roots(global_State *g, lua_State *L)' \
  'gc2_scan_pending_roots(global_State *g)' \
  'if (!sweep_minor)' \
  'LJ_GC2_HS_ALLOC_WHITE : LJ_GC2_HS_ALLOC_BLACK' \
  'lj_gc_sweep_gc2_unmarked(global_State *g)' \
  'gc_chain_splice(p, o)' \
  'gc2_unlink_root_obj(g, o);' \
  'tg->alloc.alloc_black =' \
  'la_load32_acq(&g->gc2.cycle_sweep_minor) == 0' \
  'la_add64_rlx(&g->gc2.major_root_scans' \
  'la_add64_rlx(&g->gc2.minor_root_scans' \
  'lj_gc2_finreg_cdata_preclaim(L, g, obj2gco(preclaim_cd)' \
  'old_survivor' \
  'lj_gc2_force_major(g);  /* First generational cycle establishes old marks. */' \
  'lj_gc2_barrier_tv_pair_g(global_State *g, GCobj *parent' \
  'lj_gc2_barrier_tvn_pair_g(global_State *g, GCobj *parent' \
  'test_jit_generational_table_store_remembered' \
  'active_ssb_last(tg) == obj2gco(parent)' \
  'gc_stats_setint(L, t, "generational"' \
  'gc_stats_setint(L, t, "cycle_sweep_minor"' \
  'gc_stats_setint(L, t, "cycle_roots_minor"' \
  'minor_cycle_requests' \
  'minor_cycle_starts' \
  'minor_sweep_deferred' \
  'minor_roots_deferred' \
  'major_root_scans' \
  'minor_root_scans' \
  'minor_survival_pct' \
  'minor_survival_major_requests' \
  'cycle_alloc_bytes' \
  'remembered_barriers' \
  'remembered_filtered' \
  'remembered_drained' \
  'collectgarbage("generational")' \
  'collectgarbage("incremental")'
do
  if ! rg -F -q "$needle" "$ROOT/src/lua.h" "$ROOT/src/lj_obj.h" \
      "$ROOT/src/lj_gc.h" "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc2.h" \
      "$ROOT/src/lj_gc2.c" "$ROOT/src/lj_api.c" \
      "$ROOT/src/lj_meta.h" "$ROOT/src/lj_meta.c" "$ROOT/src/lj_tg.c" \
      "$ROOT/src/lib_base.c" \
      "$ROOT/tests/t-gc-generational-mode.lua" "$ROOT/tests/t-gc-stats.lua" \
      "$ROOT/tests/t-gc2-alloc-account.c" "$ROOT/tests/t-gc2-traverse.c"; then
    echo "guardrail: missing M10 generational marker: $needle" >&2
    exit 1
  fi
done

if ! rg -F -q 'm10_generational.sh' "$ROOT/tools/ci/m9_m10_gc.sh"; then
  echo "guardrail: m10_generational.sh is not wired into the M9/M10 aggregate" >&2
  exit 1
fi

if ! awk '
  /uint32_t lj_gc_sweep_gc2_unmarked\(global_State \*g\)/ { inroot = 1 }
  inroot && /gc_chain_splice\(p, o\)/ { splice = NR }
  inroot && /gc2_free_unmarked_obj\(g, o\)/ { free = NR }
  inroot && /^}/ { rootok = splice && free && splice < free; inroot = 0 }
  /static uint32_t gc2_sweep_arena_bodies\(global_State \*g,/ { inarena = 1 }
  inarena && /gc2_unlink_root_obj\(g, o\)/ { unlink = NR }
  inarena && /gc2_free_unmarked_obj\(g, o\)/ { afree = NR }
  inarena && /^}/ { arenaok = unlink && afree && unlink < afree; inarena = 0 }
  END { exit rootok && arenaok ? 0 : 1 }
' "$ROOT/src/lj_gc.c"; then
  echo "guardrail: GC2 sweep must unlink root-list objects before free/finalizer enqueue" >&2
  exit 1
fi

"$ROOT/src/luajit" -joff "$ROOT/tests/t-gc-generational-mode.lua"
"$ROOT/src/luajit" "$ROOT/tests/t-gc-generational-mode.lua"

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-gc2-alloc-account.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
timeout 20s "$OUT"

echo "M10 generational mode guard passed"
