# Weak legacy benchmark stats

Date: 2026-06-20

## Context

The benchmark GC stats report prints a curated telemetry subset after scaled
benchmark runs. After exporting `weak_legacy_skipped` through
`collectgarbage("stats")`, the weak legacy bridge counters had enough public
coverage to include them in that benchmark report alongside finalizer and
FINREG bridge counters.

## Change

- Added `weak_legacy_skipped`, `weak_legacy_fallbacks`, and
  `weak_legacy_backfills` to `aux/bench/bench_mt.lua`'s GC stats report.
- Tightened the M9 benchmark smoke guard to require the new
  `weak_legacy_skipped=` output instead of only the generic `GC stats:` header.

## Verification

Passed:

- `tools/ci/lua_test.sh m9_bench_smoke`
