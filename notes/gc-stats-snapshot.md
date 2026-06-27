# GC Stats Snapshot

## Summary

`collectgarbage("stats")` now builds its Lua result table from a
`GC2StatsSnapshot` populated by `lj_gc2_stats_snapshot()`. The public table
schema stays unchanged, including poll-ack latency buckets and all existing
GC2 telemetry counters, but the acquire-load boundary for GC2 state now lives
inside `lj_gc2.c`.

`collectgarbage("workers")` also queries the parked-worker count through
`lj_gc2_workers_count()` instead of reading the raw worker counter helper from
`lib_base.c`.

## Guardrail

`tools/ci/m9_gc_stats.sh` requires the snapshot API and rejects direct GC2
helper reads in the stats table builder. `tools/ci/m10_generational.sh` now
checks that the snapshot, not `lib_base.c`, queries
`lj_gc2_minor_roots_active()` for `cycle_roots_minor`.
