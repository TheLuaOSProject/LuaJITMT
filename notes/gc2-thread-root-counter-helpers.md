# GC2 thread-root counter helper slice

This slice routes GC2 thread-root and suspended-thread scan telemetry through
helper accessors:

- `gc2_tg_thread_roots_*()`
- `gc2_tg_cur_roots_*()`
- `gc2_tg_trace_roots_*()`
- `gc2_thread_scan_claims_*()`
- `gc2_thread_scan_busy_*()`
- `gc2_thread_scan_requeues_*()`
- `gc2_thread_scan_owner_scans_*()`
- `gc2_thread_scan_needscan_*()`
- `gc2_thread_scan_owner_needscans_*()`
- `gc2_thread_scan_dirty_misses_*()`

`lj_gc2.c` now initializes and updates those counters through relaxed helper
stores/adds. `m3_gc2_scaffold` and `m9_gc_stats` own the observable coverage.
Production access in `lj_gc2.c` and `lib_base.c` must stay behind the
documented helper surface instead of source-text matching.

This is concurrent-GC root-scan hygiene for the current owner-side coroutine
scan handoff and per-TG root bridge. It does not change the semantics of the
claim/requeue/owner-scan protocol.
