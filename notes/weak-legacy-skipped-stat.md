# Weak legacy skipped stat

Date: 2026-06-20

## Context

`GC2State.weak_legacy_skipped` records cases where GC2 weak processing made the
legacy weak clear pass unnecessary. The counter existed and traversal/phase
tests asserted it, but `collectgarbage("stats")` only exported the fallback and
backfill side of the same bridge.

## Change

- Exported `weak_legacy_skipped` through `collectgarbage("stats")`.
- Added the key to `t-gc-stats.lua` with the same monotonicity guard as the
  adjacent weak legacy counters.

## Verification

Passed:

- `tools/ci/lua_test.sh m9_gc_stats`
- `tools/ci/lua_test.sh m8_weak`
