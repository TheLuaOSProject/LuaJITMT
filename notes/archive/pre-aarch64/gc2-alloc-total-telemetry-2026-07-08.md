2026-07-08 GC2 allocation-total telemetry
=========================================

`threading.gcstats()` now exposes `alloc_total_bytes`, a monotonic count of
flushed mutator allocation bytes. It increments in `lj_gc2_flush_alloc()` next
to `alloc_since_trigger`, but unlike the pacing counter it is not reset when a
GC2 cycle starts.

This is telemetry only. GC pacing still uses `alloc_since_trigger`,
`cycle_alloc_bytes`, `trigger_bytes`, and `hard_bytes`; the monotonic field
exists so benchmark and cadence gates can compute allocation deltas across an
active cycle without reconstructing them from resettable cycle-local fields.

The immediate user is `m9_trace_hard_assist_cadence`, which measures traced
closure allocation volume while allowing a GC2 cycle to begin during the probe.
