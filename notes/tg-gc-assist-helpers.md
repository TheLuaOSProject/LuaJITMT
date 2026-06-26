TG GC assist helper surface
===========================

Status: implemented and guarded.

Changes:

- Added `lj_tg_gc_assist_*` helpers around `TGState.gc_assist`.
- Kept the latch as a TG-owned reentry guard: acquire-load before entering
  `lj_gc2_assist()` and relaxed owner-local stores for set/clear.
- Routed production GC2 bounded-assist entry and exit through the helper layer.
- Extended `tools/ci/m3_gc2_worker_scheduler.sh` to reject raw production
  `gc_assist` access outside `src/lj_tg.h`.

Validation:

- `make -C src -j$(nproc)`
- `tools/ci/m3_gc2_worker_scheduler.sh`
- `tools/ci/m6_jit_alloc_account.sh`
- `tools/ci/m0_source_guard.sh`
