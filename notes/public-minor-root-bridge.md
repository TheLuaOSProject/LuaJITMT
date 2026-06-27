# Public minor root bridge

Date: 2026-06-20

## Problem

M10 public minor cycles chose the minor root scanner, but the legacy atomic
mark path still called `lj_gc_arena_markobj()` / `lj_gc_arena_markmem()` while
GC2 was in `LJ_GC2_MARK`. Those bridge calls marked full legacy roots into GC2
before `lj_gc2_scan_cycle_roots()` ran, so public minor cycle counters could
advance while the effective root policy was still full-major.

## Fix

`src/lj_gc2.c` now owns the predicate used by the legacy mark bridge through
`lj_gc2_legacy_mark_suppressed()`. That predicate suppresses GC2 marking from
legacy mark callbacks only when:

- GC2 phase is `LJ_GC2_MARK`
- the current cycle has `cycle_roots_minor != 0`

Legacy marking still runs. Direct GC2 minor root scans, remembered-set scans,
thread/TG roots, and major-cycle bridge marking are unchanged. `src/lj_gc.c`
only calls the GC2 predicate from `lj_gc_arena_markobj()` /
`lj_gc_arena_markmem()`; the M10 guard rejects reintroducing the local
phase/minor-root test there.

`LJ_GC2_PARANOIA` full-root fixpoint checks now return early for true minor
cycles. The existing paranoia oracle checks full legacy roots/strtab/rawroots,
which is intentionally not the minor root policy. The minor/major coverage is
kept by `t-gc2-paranoia`: it runs a true minor, then a major, then verifies the
stale-mark diff is zero.

## Regression

`tests/t-gc2-alloc-account.c` adds
`test_public_minor_skips_legacy_registry_roots()`.

The test:

1. Pre-creates a registry integer slot before the full-major baseline.
2. Enables generational mode and forces a full GC to enable minor gates.
3. Raw-updates that existing registry slot to point at a young table without a
   remembered barrier.
4. Starts a public `lj_gc_step()` minor with `stepmul = 1`.
5. Asserts major starts do not advance, minor request/start counters do advance,
   `cycle_roots_minor == 1`, and the registry-only young table remains unmarked
   by GC2.

## Verification

Passed:

- `tools/ci/lua_test.sh m3_gc2_paranoia`
- `tools/ci/lua_test.sh m10_generational`
- `tools/ci/lua_test.sh m3_gc2_scaffold`

Follow-up cleanup:

- The minor-root legacy mark suppression predicate now lives behind
  `lj_gc2_legacy_mark_suppressed()`, and `tools/ci/m10_generational.sh` guards
  that boundary.
  Verification: `tools/ci/m10_generational.sh`, `tools/ci/m9_m10_gc.sh`, and
  `tools/ci/m3_gc2_paranoia.sh` passed.
- The raw `cycle_roots_minor` predicate is now exposed to non-GC2 production
  code only through `lj_gc2_minor_roots_active()`. The legacy paranoia oracle
  and base GC stats use that helper for true minor-cycle state, while M10
  rejects direct raw `gc2_cycle_roots_minor_acq(g)` reads in `src/lj_gc.c` and
  `src/lib_base.c`.
