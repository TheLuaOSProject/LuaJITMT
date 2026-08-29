# Full-GC SMR retire drain

`collectgarbage("collect")` can shrink lockless side tables during the final
sweep, especially the string intern table after workloads that create many
short-lived keys. The old table headers are SMR-retired rather than freed
immediately, so a full collection must also advance a completed safepoint epoch
after it reaches `GCSpause`.

Without that drain, a later unrelated safepoint such as `jit.flush()` reclaimed
about 1.5 MiB of retired string-table headers after a `tab_insert_newkey` style
workload. That delayed reclaim kept the heap inflated between benchmark samples
and pushed the fresh-key table benchmark well above the stock comparison guard.

The fix keeps the normal SMR readiness rules: reclaim still only runs when the
legacy collector is paused, GC2 is idle, no GC2 worker or assist is active, and
weak-table activity is quiescent. `lj_gc_fullgc()` now performs one bounded
handshake before republishing idle pacing, and `lj_gc2_sweep_to_idle()` releases
`worker_active` before the close handshake so completed cycles may drain retired
side tables at the epoch they just closed.

Telemetry now exposes `threading.gcstats().smr_reclaim_runs` and
`threading.gcstats().smr_reclaimed` so tests can assert that full GC actually
reclaims retired side-table generations instead of depending only on byte-count
heuristics.

The same investigation exposed a separate fresh-key barrier problem: `newkey`
publication used the broad `lj_gc_pubtab()` helper, which made GC2 requeue the
entire table after every inserted key during MARK. A 20k fresh-key table then
forced the next full collection to traverse the same growing table about once
per key. New-key insertion now publishes the key edge with `lj_gc_pubtabkey()`
and keeps the legacy table back-barrier only for classic color correctness.
