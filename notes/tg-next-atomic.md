TGState next_tg link helpers
============================

Slice
-----

- Added `lj_tg_next_acq()` and `lj_tg_next_rel()` in `src/lj_tg.h`.
- Routed TG list CAS publication, dead-node reclaim splice/unlink, owner
  lookup, GC arena sweep traversal, GC2 accounting/root/sweep traversals,
  safepoint handshake scans, and threading registration checks through the
  helpers.
- Added a scoped guard in `tools/ci/m3_safepoint_handshake.sh` to reject raw
  `TGState.next_tg` access in production TG-list users.
- Extended that guard over the C fixtures that intentionally walk or manually
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

- `global_State.gc2.tg_list` remains the explicit acquire/CAS root. This slice
  centralizes the per-node link discipline only.
