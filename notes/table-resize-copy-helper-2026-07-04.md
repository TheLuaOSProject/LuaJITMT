# Table Resize Copy Helper

2026-07-04

- `lj_tab_resize()` now copies old hash-generation slots through
  `tab_resize_copy_hash_slot()`, an idempotent helper that owns one source
  `Node` at a time.
- The helper keeps the existing owner-driven resize semantics: it waits out
  transient `LJ_TKEYLOCK`, freezes old values with `FORWARD` when retiring an
  old generation, skips nil/forward/dead snapshots, and migrates into the
  successor with put-if-absent semantics.
- This is not a new lock or temporary path. It extracts the unit that a future
  cooperative resize cursor/helper can run independently while preserving the
  current publication order.
- `m5_tab_resize_copy_helper` covers the helper-level invariants: repeated copy
  does not clobber a newer successor value, a frozen source slot is a no-op on a
  second helper call, and a `KEYLOCK` source key is copied only after publication.

Follow-up:

- Move array-tail and colocated-array migration through similarly idempotent
  helper units before introducing shared copy cursors.
