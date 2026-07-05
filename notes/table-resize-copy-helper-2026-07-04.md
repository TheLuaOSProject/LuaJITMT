# Table Resize Copy Helper

2026-07-04

- `lj_tab_resize()` now copies old hash-generation slots through
  `tab_resize_copy_hash_slot()`, an idempotent helper that owns one source
  `Node` at a time.
- Array migration now goes through `tab_resize_copy_array_slot()`, the matching
  one-slot helper for same-index array copy and array-tail rehash.
- The helpers keep the existing owner-driven resize semantics: hash copy waits
  out transient `LJ_TKEYLOCK`, retiring generations freeze old values with
  `FORWARD`, invalid/dead snapshots are skipped, and successor writes use
  put-if-absent semantics.
- Array copy keeps separated-array and colocated-array behavior distinct.
  Owner-side separated-array copy keeps old live slots visible while the
  generation is marked retiring. Same-index helpers that catch the retiring
  generation copy into the successor and then best-effort forward the old slot,
  so later stale readers/writers can hop instead of parking on the retiring
  generation. Colocated arrays freeze copied/tail slots, including nil slots, so
  stale snapshots cannot accept writes after a split or shrink.
- This is not a new lock or temporary path. It extracts the unit that a future
  cooperative resize cursor/helper can run independently while preserving the
  current publication order.
- `m5_tab_resize_copy_helper` covers the helper-level invariants: repeated hash
  copy does not clobber a newer successor value, a frozen source slot is a no-op
  on a second helper call, a `KEYLOCK` source key is copied only after
  publication, array copy is idempotent, array-tail values rehash correctly, and
  nil-freeze mode stays explicit.
- `m5_tab_forward_filter` covers the matching reader side: owner-side
  separated-array resize snapshots retain old values until publication, while
  explicit `FORWARD` handoff markers still make readers follow/wait for the
  successor.

Follow-up:

- Introduce shared resize copy cursors on top of these per-slot helpers.
