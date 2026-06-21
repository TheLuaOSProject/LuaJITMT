# GC2 Weak Snapshot State Helper Slice

This slice routes the atomic weak snapshot state through helper accessors:

- `gc2_weak_count_*()` owns the MPSC reservation count for weak-table snapshot
  slots.
- `gc2_weak_scan_cursor_*()` owns the bounded read-only weak scan cursor.
- `gc2_weak_clear_cursor_*()` owns the bounded weak clear cursor and coverage
  proof cursor.

Ordering:
- Init/fini and mark-begin reset use relaxed stores while the owner is
  establishing a fresh snapshot boundary.
- Weak table discovery reserves slots with relaxed fetch-add, then publishes
  each slot through the existing queue-slot release store and ready-byte release
  store.
- Snapshot readers acquire the count and ready prefix. Scan/clear workers claim
  cursor ranges with acquire-release CAS.

Guarding:
- `tools/ci/m8_weak.sh` now requires all nine helper definitions.
- The same guard rejects raw production access to `weak_count`,
  `weak_scan_cursor`, and `weak_clear_cursor` in `lj_gc2.c`.
- Pointer/capacity ownership for `weak_stack` and `weak_ready` remains a
  separate follow-up slice.

Validation:
- `tools/ci/m8_weak.sh` passed.
- `tools/ci/m3_gc2_paranoia.sh` passed.
- `tools/ci/m10_generational.sh` passed.
- `tools/ci/m9_gc_stats.sh` passed.
- `tools/ci/m0_source_guard.sh` passed.
- Raw production weak snapshot state access scan passed.
- `git diff --check` passed.
