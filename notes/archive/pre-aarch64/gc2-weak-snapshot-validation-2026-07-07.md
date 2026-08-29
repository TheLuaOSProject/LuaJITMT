# GC2 Weak Snapshot Validation

Date: 2026-07-07

## Context

GC2 weak processing drains MPSC-published snapshot slots, overflow nodes, and
legacy weak-list bridge tables. These are queue-like sources: by the time a
worker or owner drains them, a candidate table pointer can be stale or otherwise
not name a live table object. Some weak paths still read `gct` directly before
proving the candidate was a table.

## Change

- Added a shared weak-table candidate helper that validates the object before
  reading or trusting its type tag.
- Routed weak snapshot lookup, bridge coverage/backfill/overflow clearing,
  strong-frontier closing, and paranoia bridge checks through that helper.
- Invalid weak snapshot entries now skip scan/clear work instead of touching
  the candidate header.

## Coverage

- `t-gc2-traverse.c` now publishes a canonical non-object pointer into a ready
  weak snapshot slot and asserts snapshot lookup, scan, and clear skip it.
- Focused `t-gc2-traverse` helper build passes.
- `tools/ci/lua_test.sh m8_weak` passes, including paranoia weak coverage.
- `tools/ci/lua_test.sh m3_gc2_scaffold` passes, including paranoia stock and
  `-Werror` matrix coverage.
- `tools/ci/lua_test.sh m9_m10_gc` passes.
