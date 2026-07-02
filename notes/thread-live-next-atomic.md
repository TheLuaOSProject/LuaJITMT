## LJThreadLive next-link helpers

Slice: lockless threading live-root list link discipline.

Changes:
- Added `lj_thread_live_next_acq()` and `lj_thread_live_next_rel()` in
  `src/lj_thr.h` beside `LJThreadLive`.
- Routed live-node allocation initialization, CAS publication, shutdown
  traversal, free-all traversal, and legacy GC live-thread root marking through
  the helpers.
- Documented the rule that `LJThreadLive.next` is a shared live-root
  publication link and must use the helper. Shutdown traversal and GC marking
  fixtures cover the behavior; CI must not enforce helper spelling by source
  search.
- Follow-up: routed the GC2 pending-root scan `gc2_scan_threading_live_roots()`
  through `lj_thread_live_next_acq()` and documented the same helper discipline
  for that scoped function body.
- Follow-up: added helper coverage for the live-root list head and per-thread
  node backpointer. `lj_thread_live_head_*()` now owns
  `global_State.threading_live` acquire/CAS/xchg operations, and
  `lj_thread_live_node_*()` owns `LJThread.live_node` publication/clearing.
  `lib_threading.c`, legacy GC, and GC2 route through those helpers, with the
  allowed raw-field sites documented beside the helper layer.
- Follow-up validation: `git diff --check`,
  `tools/ci/lua_test.sh m4_threading_shutdown`,
  `tools/ci/lua_test.sh m4_threading_api`, and
  `tools/ci/lua_test.sh m3_gc2_scaffold` passed.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/lua_test.sh m4_threading_shutdown`
- `tools/ci/lua_test.sh m4_threading_api`
- `tools/ci/lua_test.sh m9_gc_stats`

Notes:
- Live-root list head, node link, and per-thread backpointer operations are
  centralized in `lj_thr.h`; helper bodies are the only covered raw field sites.
