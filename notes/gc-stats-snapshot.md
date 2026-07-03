# GC Stats Snapshot

## Summary

`threading.gcstats()` now builds its Lua result table from a
`GC2StatsSnapshot` populated by `lj_gc2_stats_snapshot()`. The public table
schema stays unchanged, including poll-ack latency buckets and all existing
GC2 telemetry counters, but the acquire-load boundary for GC2 state now lives
inside `lj_gc2.c`.

`threading.gcworkers()` also queries the parked-worker count through
`lj_gc2_workers_count()` instead of reading the raw worker counter helper from
`lib_base.c`.

## Invariant check

`tools/ci/m9_gc_stats.sh` is the stats-table owner: it requires the snapshot
API and rejects direct GC2 helper reads in the stats table builder.
`tools/ci/m10_generational.sh` still checks that the snapshot records
`cycle_roots_minor` through `lj_gc2_minor_roots_active()`.
