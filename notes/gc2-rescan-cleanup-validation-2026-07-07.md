# GC2 Rescan Cleanup Validation

Date: 2026-07-07

## Context

Grey/SSB queue cleanup can see stale entries after concurrent publication,
retirement, or queue discard. The table-rescan cleanup helpers still read
`o->gch.gct` directly before proving that a candidate queue object was live.

## Change

- Split table-rescan pending cleanup into a validated wrapper and a typed helper.
- Routed `gc2_rescan_pending_clear_if_table()` through the GC2 object validator
  before clearing `LJ_GC_NEEDSCAN` or decrementing the pending-table counter.
- Routed `gc2_rescan_pending_clear_cycle()` through the same validator and uses
  the validated type tag for both table and non-table traversable cleanup.
- Added test helper entry points so regression coverage can exercise these
  cleanup paths with a canonical non-object pointer.

## Coverage

- `t-gc2-markbits.c` now sends a canonical non-object pointer through both
  rescan-cleanup helpers.
- Focused `t-gc2-markbits` helper build passes.
- `tools/ci/lua_test.sh m3_gc2_scaffold` passes, including paranoia stock and
  `-Werror` matrix coverage. One earlier scaffold attempt hit a paranoia stock
  segfault, but direct paranoia stock retries and the full scaffold rerun passed.
- `tools/ci/lua_test.sh m8_weak` passes.
- `tools/ci/lua_test.sh m9_m10_gc` passes.
