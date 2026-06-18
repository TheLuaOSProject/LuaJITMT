#!/bin/sh
# Guard the M9 benchmark-facing collectgarbage("stats") counter surface.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

make -C "$ROOT/src" >/dev/null

for needle in \
  'collectgarbage("stats")' \
  'static void gc_stats_push(lua_State *L)' \
  'cycle_starts' \
  'poll_ack_samples' \
  'poll_ack_latency_max_ns' \
  'safepoint_note_ack_latency(global_State *g)' \
  'assist_runs' \
  'sweep_owner_runs' \
  'sweep_live_updates' \
  'weak_legacy_backfills' \
  'finalizer_queued'
do
  if ! rg -F -q "$needle" "$ROOT/src/lib_base.c" \
      "$ROOT/src/lj_obj.h" "$ROOT/src/lj_safepoint.c" \
      "$ROOT/tests/t-gc-stats.lua" "$ROOT/plan/13_testing_and_benchmarks.md"; then
    echo "guardrail: missing GC stats marker: $needle" >&2
    exit 1
  fi
done

"$ROOT/src/luajit" -joff "$ROOT/tests/t-gc-stats.lua"
"$ROOT/src/luajit" "$ROOT/tests/t-gc-stats.lua"

echo "M9 GC stats guard passed"
