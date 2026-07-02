# GC2 worker counter helper slice

This slice routes parked-worker scheduler telemetry through helper accessors:

- `gc2_worker_runs_*()`
- `gc2_worker_grey_drained_*()`
- `gc2_worker_ssb_converted_*()`
- `gc2_worker_weak_drained_*()`
- `gc2_worker_idle_declares_*()`
- `gc2_worker_busy_retries_*()`
- `gc2_worker_wakes_*()`
- `gc2_worker_parks_*()`
- `gc2_worker_async_progress_*()`

Runtime producers in `lj_gc2.c` use relaxed helper stores/adds for
initialization and worker progress publication. `threading.gcstats()`
reads the same counters through acquire helpers.

`tools/ci/m3_gc2_worker_scheduler.sh` now requires the helper triplets and
documents why raw production access to these worker counter fields in `lj_gc2.c` and
`lib_base.c`.

This is scheduler state hygiene for the current parked-worker bridge. True
multi-worker marking, per-worker deque ownership, and scheduler-owned phase
completion remain separate concurrent-GC work.
