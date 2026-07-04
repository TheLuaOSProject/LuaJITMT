# Stock benchmark threshold

2026-07-03:

- `m9_bench_stock_compare` now defaults to a `3.0x` stock geomean ceiling
  instead of `100x`.
- The comparison runs the built fork against an installed stock LuaJIT binary
  found by `LJ_BENCH_STOCK_BIN` or autodetection. It is not a self-comparison.
- Current default filters are below `3.0x` against stock in local runs, with
  `closures_upval` still the widest known gap. The ceiling leaves room for that
  known work item and CI variance while still catching the historical
  table-store and closure-allocation cliffs.
- Tighter or broader release/local runs can still set `LJ_BENCH_STOCK_MAX`,
  `LJ_BENCH_STOCK_FILTERS`, and `LJ_BENCH_STOCK_SCALE`.
