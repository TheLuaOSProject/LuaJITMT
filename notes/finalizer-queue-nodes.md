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
through `lj_gc2_finalizer_mark_queued()` callbacks after claiming the finalizer
owner and draining the producer stack.

## Coverage

- `tests/t-gc2-phase.c` now asserts enqueue, drain, and dequeue preserve a queued
  object's existing `gcw` link.
- `tests/t-gc2-worker-scheduler.c` publishes the synthetic legacy sweep-ready
  boundary explicitly for worker-owned sweep setup.
- `tests/t-gc2-paranoia.c` does the same for its hand-driven minor sweep cycle.
