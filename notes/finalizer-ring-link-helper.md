# Finalizer Ring Link Helper

## 2026-06-20

Problem:
- GC2 finalizer ring enqueue/dequeue already used acquire loads for ring
  neighbors, but two ring-splice writes still spelled release `GCRef` stores
  directly through `lj_obj_gcwref()`.
- That left the finalizer ring as a special case compared with the rest of the
  GC2 finalizer MPSC/ring link publication path.

Fix:
- `lj_gc2_finalizer_drain_owned()` now updates `oldtail->nextgc` through
  `lj_obj_setgcwrel(oldtail, rev)`.
- `lj_gc2_finalizer_dequeue_owned()` acquire-loads the dequeued node's successor
  and release-publishes it through `lj_obj_setgcwrel(tail, next)`.
- Finalizer-ring next-link release stores for `oldtail` and `tail` must stay
  behind the documented helper surface; observable coverage lives in the GC2
  worker and weak/finalizer fixtures instead of source-text matching.

Verification:
- `tools/ci/lua_test.sh m3_gc2_worker_scheduler m8_weak` passed.
