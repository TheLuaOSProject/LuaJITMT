#!/bin/sh
# Guard the M9 benchmark-facing collectgarbage("stats") counter surface.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

make -C "$ROOT/src" >/dev/null

for needle in \
  'collectgarbage("stats")' \
  'static void gc_stats_push(lua_State *L)' \
  'static TValue *gc_stats_storetv_str(lua_State *L, GCtab *t, const char *name,' \
  'static TValue *gc_stats_storetv_int(lua_State *L, GCtab *t, int32_t key,' \
  'gc_stats_storetv_str(L, t, "poll_ack_latency_buckets", &tv)' \
  'lj_gc_pubtabobj(L, t, bt)' \
  'lj_gc_pubtab(L, t)' \
  'cycle_starts' \
  'minor_cycle_starts' \
  'poll_ack_samples' \
  'poll_ack_latency_max_ns' \
  'poll_ack_latency_buckets' \
  'LJ_GC2_HS_LATENCY_BUCKETS' \
  'safepoint_note_ack_latency(global_State *g)' \
  'poll_ack_p99_ns' \
  'assist_runs' \
  'sweep_owner_runs' \
  'sweep_live_updates' \
  'major_root_scans' \
  'minor_root_scans' \
  'weak_legacy_backfills' \
  'weak_keys_marked' \
  'finreg_cdata_sweep_queued' \
  'finreg_cdata_pweak_root_fallbacks' \
  'finreg_cdata_order_fallbacks' \
  'finreg_udata_registered' \
  'finalizer_queued' \
  'finalizer_mpsc_drained' \
  'finalizer_spawn_deferrals'
do
  if ! rg -F -q "$needle" "$ROOT/src/lib_base.c" \
      "$ROOT/src/lj_obj.h" "$ROOT/src/lj_safepoint.c" \
      "$ROOT/tests/t-gc-stats.lua" "$ROOT/plan/aux/bench/bench_mt.lua" \
      "$ROOT/plan/13_testing_and_benchmarks.md"; then
    echo "guardrail: missing GC stats marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'copyTVrel\(L, lj_tab_set(str|int)\(L, (t|bt)|lj_tab_storetab\(L, lj_tab_setstr\(L, t,' \
    "$ROOT/src/lib_base.c"; then
  echo "guardrail: GC stats table fields must be CAS-published" >&2
  exit 1
fi

"$ROOT/src/luajit" -joff "$ROOT/tests/t-gc-stats.lua"
"$ROOT/src/luajit" "$ROOT/tests/t-gc-stats.lua"

echo "M9 GC stats guard passed"
