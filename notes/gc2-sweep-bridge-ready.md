# GC2 sweep bridge-ready latch

This slice adds an explicit GC2 latch for the sweep bridge close boundary.

The GC bridge reports that the root sweep has reached its close point through
`lj_gc2_sweep_bridge_boundary_reached(g)`. GC2 then owns the raw
`lj_gc2_sweep_bridge_ready(g)` latch publication after post-root cleanup and
the boundary-lazy arena preparation step have had a chance to execute.
`P_SWEEP -> P_IDLE` publication through `lj_gc2_sweep_to_idle(g)` now refuses
to close until that latch is set, in addition to the existing finalizer,
sweep-prepare, and pending-sweep predicates.

The latch is reset on initialization, preserve abort, and each real
`WEAK -> SWEEP` publication. The parked worker can still drain traversable arena
sweep work before the latch, but cannot cause an early idle publication before
string/root sweep is complete.

The worker does not yet publish `P_IDLE` itself. `lj_gc2_sweep_to_idle()` runs a
safepoint handshake, and parked GC workers are not attached TGs with their own
handshake identity. Worker-owned idle publication should wait until the worker
pool has a real scheduler/TG identity instead of relying on main-TG fallback.

`m3_gc2_worker_scheduler` owns the observable sweep bridge behavior. The latch
field and raw latch publication helper must stay behind the documented GC2
helper surface; that ownership rule is documented here instead of source-text
matching.
