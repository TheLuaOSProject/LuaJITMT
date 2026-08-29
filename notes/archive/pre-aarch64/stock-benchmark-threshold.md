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

2026-07-13 b1.2.0 scope revision:

- The default ceiling is temporarily `100.0x`. b1.2.0 is a GC2/JIT
  functionality beta, so ordinary multi-x regressions are reported but do not
  fail the release gate. Timeouts, runaway resource use, and roughly 100x
  throughput cliffs remain blockers.
- The prior `3.0x` ceiling and the wider parity-or-better target are not
  abandoned; restoring and then tightening that guard is explicit b1.2.1
  performance work. `LJ_BENCH_STOCK_MAX` continues to support stricter local
  runs in the meantime.
