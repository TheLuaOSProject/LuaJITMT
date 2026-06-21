# FINREG Preclaim Slot Helpers

## 2026-06-20

Problem:
- Cdata FINREG preclaim consumers acquired object slots through helper surfaces,
  but producer publication, clearing, and resize/compaction still spelled direct
  `GCRef` loads/stores for the preclaim object vector.
- The side vector relies on the finalizer value being release-copied before the
  object slot becomes the ready marker, so direct object-slot writes made that
  publication contract easy to bypass.

Fix:
- `gc2_finclaim_publish()`, `gc2_finclaim_clear()`, and
  `gc2_finclaim_copy_slot()` now use `gc2_queue_slot_store_rel()`,
  `gc2_queue_slot_clear_rel()`, and `gc2_queue_slot_load_acq()` for preclaim
  object slots.
- `tools/ci/m7_ffi_finreg.sh` rejects raw preclaim object-slot loads/stores in
  `src/lj_gc2.c`.
- Follow-up state helper work routes the preclaim vector pointers, capacity,
  head cursor, and count publisher through `gc2_finreg_cdata_preclaim_*()`
  helpers in both `lj_gc.c` and `lj_gc2.c`, while keeping the slot publication
  order unchanged.

Verification:
- `tools/ci/m7_ffi_finreg.sh` and `tools/ci/m8_weak.sh` passed.
