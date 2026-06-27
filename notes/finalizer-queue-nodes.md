# GC2 finalizer queue nodes

## Context

The GC2 finalizer bridge used each queued object's `gcw` link as both the MPSC
producer-stack link and the single-consumer ring link. That kept the bridge
compact, but it also rewrote a queued object's normal root/list link while the
object was pending finalization.

## Change

The finalizer queue now stores dedicated `GC2FinalizerNode` records. The MPSC
producer stack and owner ring link those nodes, and each node points at the
queued `GCobj`.

Queue node allocation uses `malloc`/`free` rather than the Lua allocator. This is
intentional: enqueue can happen from producer pthreads that do not own the Lua
allocator or a collector TG, so losing Lua allocator accounting here is safer
than taking allocator ownership implicitly.

Legacy GC, GC2 root scans, and paranoia checks now mark queued finalizer objects
through `lj_gc2_finalizer_mark_all()` callbacks. GC2 owns claiming the
finalizer owner, draining the producer stack, and walking the stable owner ring.
Dispatch-time consumption enters through GC2's internal
`lj_gc2_finalizer_dispatch_one()` for the same owner/drain/dequeue/release
sequence. Public dispatch-all/step entry points no longer accept a callback
function pointer; GC2 owns dequeued-object routing into cdata/userdata FINREG
dispatch and the protected callback runner.
Close-time drain-all uses `lj_gc2_finalizer_dispatch_all()`, keeping the
blocking drain/pending/dispatch loop on the GC2 side of the queue boundary.
Incremental `GCSfinalize` work uses `lj_gc2_finalizer_step()`, so queue-pending
checks, trace deferral, per-finalizer cost accounting, and finalizer-spawn
deferral are also kept behind the GC2 finalizer boundary.

## Coverage

- `tests/t-gc2-phase.c` now asserts enqueue, drain, and dequeue preserve a queued
  object's existing `gcw` link.
- `tests/t-gc2-worker-scheduler.c` publishes the synthetic legacy sweep-ready
  boundary explicitly for worker-owned sweep setup.
- `tests/t-gc2-paranoia.c` does the same for its hand-driven minor sweep cycle.
