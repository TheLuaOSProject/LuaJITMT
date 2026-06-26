GC2 SSB next-link helpers
=========================

Slice
-----

- Added `lj_gc2_ssb_next_acq()` and `lj_gc2_ssb_next_rel()` beside the
  `GC2SSBNode` definition in `src/lj_tg.h`.
- Routed SSB node activation, MPSC publication, free-list pop/push, partial
  publish-list stitching, and published-list drain through the helper pair.
- Added a guard in `tools/ci/m3_gc2_worker_scheduler.sh` to reject raw
  `GC2SSBNode.next` access in `src/lj_gc2.c`.
- Follow-up: routed embedded `TGState.ssb_node[]` initialization in
  `src/lj_tg.c` through `lj_gc2_ssb_next_rel()` and extended the same guard to
  reject raw `ssb_node[].next` initialization.

Validation
----------

- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m3_gc2_worker_scheduler.sh`
- `tools/ci/m3_safepoint_handshake.sh`
- `tools/ci/m9_gc_stats.sh`

Notes
-----

- Follow-up slices now route `TGState` SSB free/cursor state and
  `global_State.gc2.ssb_head` through dedicated helpers. This original slice
  centralized the per-node link discipline.
