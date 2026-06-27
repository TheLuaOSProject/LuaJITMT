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
- Follow-up: added helper coverage for the live-root list head and per-thread
  node backpointer. `lj_thread_live_head_*()` now owns
  `global_State.threading_live` acquire/CAS/xchg operations, and
  `lj_thread_live_node_*()` owns `LJThread.live_node` publication/clearing.
  `lib_threading.c`, legacy GC, and GC2 route through those helpers, and the
  shutdown guard rejects raw covered access to both fields.
- Follow-up validation: `git diff --check`,
  `tools/ci/m4_threading_shutdown.sh`, `tools/ci/m4_threading_api.sh`, and
  `tools/ci/m3_gc2_scaffold.sh` passed.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m4_threading_shutdown.sh`
- `tools/ci/m4_threading_api.sh`
- `tools/ci/m9_gc_stats.sh`

Notes:
- Live-root list head, node link, and per-thread backpointer operations are
  centralized in `lj_thr.h`; helper bodies are the only covered raw field sites.
