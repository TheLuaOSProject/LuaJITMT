# Finalizer Queue State Helpers

GC2 finalizer queue state now has an explicit helper surface for the
producer-published MPSC stack, single-consumer ring tail, dispatch owner/active
state, and MPSC-drained counter.

Routed users:

- finalizer enqueue/drain/dequeue;
- legacy and GC2 finalizer ring marking;
- finalizer owner enter/leave and pending checks;
- idle worker finalizer-drain polling/accounting.
- phase/traversal fixtures that inspect queue and owner state.

Coverage:

- `m3_gc2_worker_scheduler` and `m8_weak` own the observable finalizer queue
  behavior. Direct runtime access to `g->gc2.finalizer_mpsc`,
  `finalizer_tail`, `finalizer_active`, `finalizer_owner_tid`, and
  `finalizer_mpsc_drained` must stay behind the documented helper surface in
  production code and fixtures.
- Follow-up counter helper work now routes queue, dequeue, guard, sweep-block,
  spawn-deferral, and release-wake telemetry through `gc2_finalizer_*()`
  helpers, and extends the worker-scheduler note documenting raw counter access
  in GC, GC2, stats, and threading code.

Validation:

- `tools/ci/lua_test.sh m3_gc2_worker_scheduler`
- `tools/ci/lua_test.sh m8_weak`
- `git diff --check`
