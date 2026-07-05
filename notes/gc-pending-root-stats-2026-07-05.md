# GC pending-root telemetry

The closure-allocation regressions are dominated by GC-side work when the
legacy collector is still coupled into a workload. Before changing that path,
`threading.gcstats()` now exposes the state needed to prove where the work is:

* `legacy_gc_state` is the classic collector state byte. It shows whether GC2 is
  running alongside a live legacy incremental phase.
* `pending_root_flushes`, `pending_root_flushed`, and
  `pending_root_flush_max` count only drains that actually move objects from
  per-TG pending-root stacks into the legacy root spine. The empty fast check in
  `lj_gc_flush_root_pending()` still returns without touching shared counters.
* `root_spine_count_cap` and `root_spine_count_capped` document the diagnostic
  ceiling used when counting the legacy root spine. A capped count means the
  root spine is already too large to count cheaply, not that it has exactly that
  many objects.

These fields are intentionally observability-only. They do not add locking or a
new collection path; they make pending-root pressure and legacy-root-spine
growth measurable before replacing the underlying bottleneck.
