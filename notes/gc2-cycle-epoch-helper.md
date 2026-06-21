# GC2 Cycle Epoch Helper Slice

This slice routes the authoritative GC2 cycle epoch through helper accessors:

- `gc2_cycle_acq()` snapshots the current epoch for sweep preparation, sweep
  owner progress, live aggregation, and thread-root scan freshness checks.
- `gc2_cycle_store_rlx()` initializes the epoch before the state is
  concurrently visible.
- `gc2_cycle_inc_acqrel()` publishes the next epoch at mark begin with an
  acquire-release CAS loop, so readers that observe the new epoch also have an
  ordered view of the current cycle's latch publication.

Root-scan paths now snapshot the epoch before scanning and publish that same
epoch with `scan_epoch`. If a new cycle starts while a stack scan is in
progress, the old epoch is published and later worker-owner freshness checks
will reject it instead of treating a stale scan as current-cycle coverage.

Guarding:
- `tools/ci/m3_gc2_worker_scheduler.sh` rejects raw production access to
  `GC2State.cycle` in `lj_gc.c`, `lj_gc2.c`, and `lj_safepoint.c`.
- The guard intentionally does not match `cycle_requests`, `cycle_starts`, or
  the minor-cycle latch fields.

Validation:
- `tools/ci/m3_gc2_worker_scheduler.sh` passed.
- `tools/ci/m3_safepoint_handshake.sh` passed.
- `tools/ci/m3_vm_safepoint.sh` passed.
- `tools/ci/m3_gc2_paranoia.sh` passed.
- `tools/ci/m10_generational.sh` passed.
- `tools/ci/m8_weak.sh` passed.
- `tools/ci/m6_jit_alloc_account.sh` passed.
- `tools/ci/m0_source_guard.sh` passed.
- Raw production `GC2State.cycle` access scan passed.
- `git diff --check` passed.
