## LJThreadLive next-link helpers

Slice: lockless threading live-root list link discipline.

Changes:
- Added `lj_thread_live_next_acq()` and `lj_thread_live_next_rel()` in
  `src/lj_thr.h` beside `LJThreadLive`.
- Routed live-node allocation initialization, CAS publication, shutdown
  traversal, free-all traversal, and legacy GC live-thread root marking through
  the helpers.
- Extended `tools/ci/m4_threading_shutdown.sh` to reject direct
  `LJThreadLive.next` access in `lib_threading.c` and in the scoped
  `gc_mark_threading_live()` body.
- Follow-up: routed the GC2 pending-root scan `gc2_scan_threading_live_roots()`
  through `lj_thread_live_next_acq()` and extended the same shutdown guard to
  cover that scoped function body.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m4_threading_shutdown.sh`
- `tools/ci/m4_threading_api.sh`
- `tools/ci/m9_gc_stats.sh`

Notes:
- `global_State.threading_live` remains the explicit acquire/CAS/xchg list head.
  This slice centralizes the per-node link only.
