# GC2 sweep legacy-ready latch

This slice adds an explicit GC2 latch for the legacy sweep close boundary.

`lj_gc2_sweep_legacy_ready(g)` is called from `lj_gc.c` only after the legacy
root sweep has reached its close point, post-root cleanup has run, and the
boundary-lazy arena preparation step has had a chance to execute. `P_SWEEP ->
P_IDLE` publication through `lj_gc2_sweep_to_idle(g)` now refuses to close until
that latch is set, in addition to the existing finalizer, sweep-prepare, and
pending-sweep predicates.

The latch is reset on initialization, preserve abort, and each real
`WEAK -> SWEEP` publication. The parked worker can still drain traversable arena
sweep work before the latch, but cannot cause an early idle publication before
legacy string/root sweep is complete.

The worker does not yet publish `P_IDLE` itself. `lj_gc2_sweep_to_idle()` runs a
safepoint handshake, and parked GC workers are not attached TGs with their own
handshake identity. Worker-owned idle publication should wait until the worker
pool has a real scheduler/TG identity instead of relying on main-TG fallback.

`tools/ci/m3_gc2_worker_scheduler.sh` now requires the latch helpers and rejects
raw production access to the latch field.
