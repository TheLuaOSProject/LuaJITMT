Active-MT shared table trace coverage, 2026-07-03:

- `m6_jit_table_store_helper` now includes secondary-TG active-MT coverage for
  previous-nil hash stores, previous-nil array stores, new dynamic hash stores,
  and new numeric array stores on non-trace-local tables.
- `m6_jit_aref_pair_boundary` now includes secondary-TG active-MT shared hash reads
  with constant and dynamic keys, alongside the existing shared array read
  coverage.
- The remaining deliberate JIT fallback is non-trace-local shared traversal:
  direct `next()`, optimized `pairs()`, and shared `ipairs_aux`. These paths
  need versioned/generation-following runtime contracts and result-shape
  handling before they can trace under concurrent resize/value churn.
- 2026-07-04: the first contract slice is pinned in `m5_tab_next_snapshot`.
  `lj_tab_vmnext_forward()` and `lj_tab_itern_forward()` now have an explicit
  cursor-helper contract: take an `LJ_KEYINDEX` cursor, copy visible key/value
  snapshots into caller-owned `TValue` storage, recompute the next cursor from
  the current table generation, and never expose KEYLOCK/FORWARD/internal table
  sentinels. The recorder still deliberately falls back for active-MT shared
  traversal until it is routed through that helper-shaped contract.
- Follow-up: recorder type prediction for `next()` now uses
  `lj_tab_vmnext_forward()` instead of scanning raw array/hash storage. This
  removes the local KEYLOCK/FORWARD/retiring-generation prediction hazard.
  Direct `next(shared, key)` now records under active MT because every call
  re-derives the cursor from the returned Lua key. Optimized `pairs()`/BC_ITERN
  still remains interpreted for non-trace-local shared tables: it carries the
  hidden `LJ_KEYINDEX` cursor across loop iterations, and reopening that path
  still fails the `traversal,nextchurn` stress without a generation/version
  guard.
- 2026-07-04 reducer: a helper-only optimized `pairs()`/BC_ITERN reopening
  traced `m6_jit_token`, but `m5_tab_resize_stress` with `traversal,nextchurn`
  corrupted an observer loop local (`count` became a function value). Adding
  array/node/asize/hmask root guards avoided the immediate corruption but
  timed out under stress from exit/re-record churn. The required contract is
  therefore a versioned cursor/epoch or equivalent result contract, not a
  naked helper call plus structural root guards.
- 2026-07-06 `ipairs_aux` reopening attempt: helper-backed indexed loads are now
  memory-safe enough to pass `tests/t-jit-secondary.lua`,
  `LJ_M5_TAB_RESIZE_STRESS_CASES=jititer ./src/luajit tests/t-tab-resize-stress.lua`,
  and full `tests/t-tab-resize-stress.lua` with a longer timeout. The clean
  `m5_tab_resize_stress` 30s gate timed out, and `-jv` showed a side-trace chain
  at `tests/t-tab-resize-stress.lua:734 -> 1` before fallback. The blocker is
  no longer sentinel exposure; it is result type/shape churn for racy mixed
  array values. Reopening shared `ipairs_aux` needs a result-shape contract or a
  trace policy that avoids side-tracing every observed value type.
