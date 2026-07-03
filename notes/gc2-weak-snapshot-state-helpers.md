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

Coverage:
- `m8_weak` owns the observable weak snapshot behavior.
- Production access to `weak_count`, `weak_scan_cursor`, and
  `weak_clear_cursor` in `lj_gc2.c` must stay behind the documented helper
  surface instead of source-text matching.
- Follow-up vector helper work now routes `weak_stack`, `weak_ready`, and
  `weak_capacity` through `gc2_weak_*()` helpers while preserving the current
  owner-quiesced resize invariant.

Validation:
- `tools/ci/lua_test.sh m8_weak` passed.
- `tools/ci/lua_test.sh m3_gc2_paranoia` passed.
- `tools/ci/lua_test.sh m10_generational` passed.
- `tools/ci/lua_test.sh m9_gc_stats` passed.
- `git diff --check` passed.
