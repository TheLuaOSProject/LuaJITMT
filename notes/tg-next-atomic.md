TGState next_tg link helpers
============================

Slice
-----

- Added `lj_tg_next_acq()` and `lj_tg_next_rel()` in `src/lj_tg.h`.
- Routed TG list CAS publication, dead-node reclaim splice/unlink, owner
  lookup, GC arena sweep traversal, GC2 accounting/root/sweep traversals,
  safepoint handshake scans, and threading registration checks through the
  helpers.
- Added a scoped guard in `tools/ci/m3_safepoint_handshake.sh` to document raw
  `TGState.next_tg` access in production TG-list users.
- Follow-up registry-helper slice routes the global `GC2State.tg_list` root
  and `GC2State.n_threads` live count through `gc2_tg_*`/`gc2_n_threads_*`
  helpers, with the same notes document why we avoiding raw production registry access.
- Extended that invariant coverage for the C fixtures that intentionally walk or manually
  publish TG-list nodes, and routed their `next_tg` reads/writes through the
  helpers too.
- While touching `t-thr-substrate`, made its attach-during-handshake fixture
  deterministic by pinning the handshake with a separate remote native TG. The
  main TG can be acknowledged directly by the handshake leader, so clearing the
  main TG's `cur_L` was no longer a reliable pending-handshake hold.

Validation
----------

- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m3_safepoint_handshake.sh`
- `tools/ci/m3_gc2_worker_scheduler.sh`
- `tools/ci/m4_threading_shutdown.sh`
- `tools/ci/m4_threading_api.sh`
- `tools/ci/m4_thr_substrate.sh`
- `tools/ci/m9_gc_stats.sh`

Notes
-----

- `TGState.next_tg` and the global `GC2State.tg_list` root now have separate
  helper surfaces: `lj_tg_next_*()` owns per-node links, while `gc2_tg_*()`
  owns the list root and CAS publication/unlink discipline.
