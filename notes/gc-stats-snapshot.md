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

## Coverage

`m9_gc_stats` is the stats-table owner: it exercises the public
`threading.gcstats()` result, including the snapshot-populated fields and
latency buckets. `m10_generational` checks that generational mode exposes the
minor-root accounting expected by the snapshot. The snapshot API requirement is
documented here and beside `lj_gc2_stats_snapshot()` instead of being enforced
by source-text matching.
