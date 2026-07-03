TG GC assist helper surface
===========================

Status: implemented and guarded.

Changes:

- Added `lj_tg_gc_assist_*` helpers around `TGState.gc_assist`.
- Kept the latch as a TG-owned reentry guard: acquire-load before entering
  `lj_gc2_assist()` and relaxed owner-local stores for set/clear.
- Routed production GC2 bounded-assist entry and exit through the helper layer.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m3_gc2_worker_scheduler` behavior/counter fixtures and code-adjacent helper docs; raw-field implementation-text inventories are not pass/fail contracts.

Validation:

- `make -C src -j$(nproc)`
- `tools/ci/m3_gc2_worker_scheduler.sh`
- `tools/ci/m6_jit_alloc_account.sh`
