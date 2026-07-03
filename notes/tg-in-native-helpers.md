TG native-state helper surface
==============================

Status: implemented and guarded.

Changes:

- Added `lj_tg_in_native_*` helpers around `TGState.in_native`.
- Kept the existing ordering: release publication on native enter/restore,
  relaxed owner-local clears, and acquire remote/callback reads.
- Routed production native enter/leave, channel/sleep clear paths, GC2 worker
  joins, safepoint remote native ack, and FFI callback re-entry checks through
  the helper layer.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m3_safepoint_handshake` behavior/counter fixtures and code-adjacent helper docs; raw-field source inventories are not pass/fail contracts.

Validation:

- `make -C src -j$(nproc)`
- `tools/ci/m3_safepoint_handshake.sh`
- `tools/ci/m3_gc2_worker_scheduler.sh`
- `tools/ci/m7_ffi_callback_runtime.sh`
- `tools/ci/m0_matrix.sh`
