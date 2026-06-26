GC2 finalizer link publication slice

- Added release-store helpers for `GCobj.nextgc` link copies and clears.
- Routed GC2 finalizer MPSC enqueue, owner drain/reversal, tail append, and
  dequeue unlink through release-published `nextgc` updates.
- Left allocation/root-list fast paths on their existing plain-prep plus
  release-CAS publication path.

Verification:

- tools/ci/lua_test.sh m8_weak
- tools/ci/lua_test.sh m3_gc2_worker_scheduler
- tools/ci/lua_test.sh m3_gc2_paranoia

Current state:

- This was superseded by `notes/finalizer-queue-nodes.md`: the GC2 finalizer
  queue no longer links through queued objects' `gcw` fields. The release-link
  work remains relevant for other object-list paths, but finalizer queue links
  now live in dedicated queue nodes.
