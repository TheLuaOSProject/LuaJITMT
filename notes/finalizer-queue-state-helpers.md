# Finalizer Queue State Helpers

GC2 finalizer queue state now has an explicit helper surface for the
producer-published MPSC stack, single-consumer ring tail, dispatch owner/active
state, and MPSC-drained counter.

Routed users:

- finalizer enqueue/drain/dequeue;
- legacy and GC2 finalizer ring marking;
- finalizer owner enter/leave and pending checks;
- idle worker finalizer-drain polling/accounting.

Guardrail:

- `tools/ci/m3_gc2_worker_scheduler.sh` rejects direct runtime access to
  `g->gc2.finalizer_mpsc`, `finalizer_tail`, `finalizer_active`,
  `finalizer_owner_tid`, and `finalizer_mpsc_drained` in `lj_gc.c` and
  `lj_gc2.c`.

Validation:

- `tools/ci/m3_gc2_worker_scheduler.sh`
- `tools/ci/m8_weak.sh`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`
