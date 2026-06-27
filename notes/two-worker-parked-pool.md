Two-worker parked pool slice
============================

Context:
- M3 still used a single parked worker and treated every positive
  `collectgarbage("workers", N)` request as one worker.
- The actual GC drain path is still intentionally serialized by
  `gc2.worker_active`; this slice does not claim full parallel marking or
  per-worker deque ownership.

Change:
- `GC2State` now stores a capped two-slot parked worker array.
- `lj_gc2_workers_set(g, n)` starts/stops an exact capped count; the old
  single-worker start helper has been removed.
- Internal `lj_gc2_worker_wake()` wakes the active parked worker count instead
  of one waiter; C harnesses use `lj_gc2_test_worker_wake()` for forced wake
  races.
- `collectgarbage("workers", 2)` now results in two active parked workers.
- A follow-up helper slice routes wake, park, contention, and async-progress
  telemetry through `gc2_worker_*()` counter helpers.

Reason:
- This creates real scheduler concurrency at the OS-thread level while keeping
  the existing single-owner drain token around the unsafe shared GC worklists.
- It gives later work an observable contention/progress surface before moving
  to per-worker deques or scheduler-owned finalizer dispatch.

Coverage:
- `tests/t-gc2-worker-scheduler.c` starts two workers, holds `worker_active`,
  wakes the pool, and asserts both workers observe contention through
  `worker_busy_retries` before the existing async mark/weak/sweep checks run.
- `tests/t-gc-workers.lua` verifies the public capped worker count behavior.
