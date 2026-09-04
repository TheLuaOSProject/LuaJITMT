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

2026-07-04 follow-up: `threading.gcstats()` also exposes bounded diagnostic
counts for the legacy root spine and the main TG traversable arena lists:
`root_spine_objects`, `root_spine_tombstones`, `arena_traversable_owned`,
`arena_traversable_needsweep`, and `arena_traversable_binmask`. These are not a
hot-path API; they exist so stability and benchmark probes can distinguish
root-spine growth, mark-entry arena reset mistakes, and free-run publication
state without reading runtime structs directly.

2026-09-04 follow-up: the main-TG arena counts and binmask now come from
owner-published scalar evidence. Remote snapshots no longer read allocator
list heads or arena nodes; the independent caps and settled meanings are
unchanged. See [the mutation/lifetime audit and regression evidence](gc-stats-arena-publication-2026-09-04.md).

## Coverage

`m9_gc_stats` is the stats-table owner: it exercises the public
`threading.gcstats()` result, including the snapshot-populated fields and
latency buckets. `m10_generational` checks that generational mode exposes the
minor-root accounting expected by the snapshot. The snapshot API requirement is
documented here and beside `lj_gc2_stats_snapshot()` instead of being enforced by the named fixtures.
