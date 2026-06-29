# GC2 worker lifecycle helper slice

This slice routes the parked-worker lifecycle words through helper accessors:

- `gc2_n_workers_*()` publishes and reads the current parked worker count.
- `gc2_worker_stop_*()` publishes and reads the stop request.
- `gc2_worker_wake_*()` publishes wake sequence increments and wraps the futex
  wait/wake word.
- `gc2_worker_started_*()` publishes startup progress and wraps the startup
  futex wait/wake word.
- `gc2_worker_exited_*()` publishes worker exit progress and wraps the exit
  futex wake word.

Runtime users in `lj_gc2.c` and the `threading.gcworkers()` query in
`lib_base.c` no longer spell ad hoc atomics or futex operations against
`GC2State.n_workers`, `worker_stop`, `worker_wake`, `worker_started`, or
`worker_exited`. `tools/ci/m3_gc2_worker_scheduler.sh` rejects future raw
production and focused-fixture access to those lifecycle fields. The focused
fixtures also route worker activity and worker telemetry assertions through the
same helper surface, so the test harness exercises the contract it guards. The
separate `worker_wakes` telemetry counter is covered by the follow-up worker
counter helper slice.

Validation:

- `tools/ci/m3_gc2_worker_scheduler.sh`
- `tools/ci/m9_gc_stats.sh`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`
