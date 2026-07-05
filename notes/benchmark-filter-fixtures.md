# Benchmark Filter Fixtures

Date: 2026-07-05

Filtered `aux/bench/bench.lua` runs are used by `m9_bench_stock_compare` to
measure one throughput surface at a time. Benchmark fixtures therefore need to
be local to the selected case: unselected fixtures can leave extra live GC
objects, trigger unrelated recorder work, and distort allocation-heavy rows.

`tab_store_existing` used to build its 8192-key fixture at chunk load time.
That meant `bench.lua closures_upval` carried the existing-key table and string
set even though the table-store benchmark was filtered out. On this fork that
inflated the fresh-build `closures_upval` stock comparison from the focused
closure/allocation path into a partially polluted GC-state measurement.

The harness now supports an optional per-benchmark setup function. The
existing-key fixture is built only when `tab_store_existing` is selected and is
kept outside the timed loop, matching the old benchmark semantics for that case
while keeping filtered runs isolated.

Fresh-build local checks after the change:

- `LJ_TEST_DISABLE_BUILD_CACHE=1 LJ_BENCH_STOCK_FILTERS='tab_insert_newkey alloc_tables closures_upval fib30' LJ_BENCH_STOCK_SCALE=0.1 LJ_BENCH_STOCK_MAX=10 LJ_BENCH_STOCK_TIMEOUT=180s LUA=luajit tools/ci/lua_test.sh m9_bench_stock_compare`
  reported `closures_upval` geomean `2.786082`.
- Default `LJ_TEST_DISABLE_BUILD_CACHE=1 LUA=luajit tools/ci/lua_test.sh m9_bench_stock_compare`
  reported `closures_upval` geomean `2.028078` and all default rows under the
  guard ceiling.
