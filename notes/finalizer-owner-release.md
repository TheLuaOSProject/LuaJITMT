Finalizer owner release slice
=============================

Context:
- M8 still has scheduler-owned finalizer dispatch work outstanding.
- Current `finalizer_active` already reserved `~0u` as a closing/busy state:
  `lj_gc2_finalizer_try_enter()` refuses to enter when it observes that value.

Change:
- `lj_gc2_finalizer_leave()` now uses the reserved sentinel for the last leave:
  CAS `1 -> ~0u`, clear `finalizer_owner_tid`, then publish
  `finalizer_active = 0`.
- Nested leaves use CAS decrement instead of fetch-sub.

Reason:
- The old last-leave order cleared `finalizer_owner_tid` before decrementing
  `finalizer_active`, creating an ownerless-active window. That was
  conservative for peers, but it made the ownership state internally
  inconsistent while scheduler-owned finalizer work is being tightened.
- Clearing owner after simply decrementing active would risk clobbering a new
  entrant's owner store. The sentinel keeps new entrants out until both fields
  are consistently reset.

Finalizer scheduler wake slice
==============================

Context:
- Finalizer publication is GC2-owned, but callback dispatch is still invoked by
  the legacy drain callers.
- A scheduler-owned dispatcher needs a real handoff point before callback
  execution can move.

Change:
- `lj_gc2_finalizer_enqueue()` now wakes the parked GC worker scheduler when a
  successful MPSC push changes the producer stack from empty to non-empty.
- The wake is not emitted for every producer push while work is already visible.

Reason:
- This keeps the existing queue ordering, finalizer owner claim, callback stack
  ownership, and close-time behavior unchanged.
- It creates an observable scheduler request at the point where finalizer work
  first becomes available.

Coverage:
- `tests/t-gc2-phase.c` starts the parked worker, enqueues three finalizer
  objects onto an empty producer stack, and asserts `worker_wakes` advances once
  while the existing drain/dequeue order checks still pass.

Worker-side queue drain slice
=============================

Context:
- Finalizer enqueue now wakes the parked worker scheduler, but workers still
  only consumed mark/weak/sweep work.
- Executing user finalizer callbacks from a GC worker is not safe yet because
  callbacks need a claimed `lua_State` and the current legacy caller stack
  ownership rules.

Change:
- `gc2_worker_drain_inner()` first tries to claim the GC2 finalizer owner and
  drain the producer MPSC stack into the existing consumer ring.
- Worker-side finalizer draining is limited to `LJ_GC2_IDLE`, so it cannot hide
  pending finalizer roots from a concurrent GC2 mark root scan.
- Worker-side finalizer draining also holds `worker_active`, so TLS-less GC
  workers cannot use the shared fallback finalizer owner as a multi-consumer
  ring-splice path.
- The worker does not dequeue objects and does not execute callbacks.
- If another finalizer owner is active, the worker backs off and preserves the
  existing legacy callback path.
- The final owner leave republishes a worker wake if MPSC work remains, closing
  the case where a worker consumed an enqueue wake while a real finalizer owner
  was still active.

Reason:
- This makes the previous enqueue wake meaningful without changing finalizer
  callback ordering, stack ownership, close-time semantics, or FINREG
  resolution.
- Pending finalizer work remains visible through `finalizer_tail`, so sweep is
  still blocked until callbacks are actually handled by the collector caller.

Coverage:
- `tests/t-gc2-worker-scheduler.c` starts the two-worker parked pool, enqueues
  two synthetic finalizer objects while idle, waits for
  `finalizer_mpsc_drained` and `worker_async_progress`, then dequeues the same
  objects through the normal GC2 finalizer queue path.
- The same fixture switches to one worker, publishes while the main TG owns
  finalizer dispatch, waits for the original wake to be consumed, then asserts
  owner leave wakes the worker and the queued objects drain in FIFO order.
