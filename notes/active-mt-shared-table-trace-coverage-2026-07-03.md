Active-MT shared table trace coverage, 2026-07-03:

- `m6_jit_table_store_helper` now includes secondary-TG active-MT coverage for
  previous-nil hash stores, previous-nil array stores, new dynamic hash stores,
  and new numeric array stores on non-trace-local tables.
- `m6_jit_aref_pair_boundary` now includes secondary-TG active-MT shared hash reads
  with constant and dynamic keys, alongside the existing shared array read
  coverage.
- The remaining deliberate JIT fallback is non-trace-local shared
  `next()`/optimized `pairs()` traversal. That path still needs a versioned or
  generation-following runtime contract before it can safely trace under
  concurrent resize/value churn.
