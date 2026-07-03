# Stability sweep 2026-07-03

Scope: follow up on older catastrophic-regression claims against current
`v2.1` after the entering-window table/JIT fixes and invariant-guidance cleanup.
No runtime changes were made in this sweep because the checked claims were
already fixed, covered, or not reproducible in current state.

## Recursive `fib30`

Current focused benchmark probes no longer reproduce the old "TRACE 1 forever"
failure. With `BENCH_SCALE=1`:

- `src/luajit plan/aux/bench/bench.lua fib30`: 9.441 ms/op.
- `/usr/bin/luajit plan/aux/bench/bench.lua fib30`: 6.622 ms/op.

The recursive trace guard also passed:

- `tools/ci/lua_test.sh m6_jit_recursive_call_unroll`

Direct `-jv` trace shape with `hotloop=56,hotexit=10` still shows the fork
recording more return traces before the up-recursion trace in one sample
(`up-recursion` at trace 19, 38 traces after two runs) than stock in that same
sample (`up-recursion` at trace 11, 26 traces after two runs). This is a
remaining performance/trace-shape difference, but not the previous endless
re-recording correctness failure. The existing `lj_trace_flush_unlink()` path
must not be replaced by scoped trace retirement: it exists so `trace_abort()`
can still self-link the unlinked return trace as the stock blacklist entry.

Independent 50-process samples with the same guard shape also stayed bounded:

- Fork: first run 17..36 traces, second run 21..40, second-run delta 3..4,
  up-recursion at trace 2..21, bad runs 0/50.
- Stock: first run 17..41 traces, second run 21..45, second-run delta 3..4,
  up-recursion at trace 2..26, bad runs 0/50.

Looped timing after one warm `fib(30)` still shows a current fork gap:

- Fork: about 7.26..7.45 ms per call across three 50-call process samples.
- Stock: about 5.33..5.60 ms per call across three 50-call process samples.

## Benchmark guard

The benchmark-regression guard is no longer purely self-referential in current
state. `m9_bench_stock_compare` autodetected `/usr/bin/luajit` and compared the
fork against stock:

- `arith_loop`: geomean 0.975000
- `fib30`: geomean 1.346050
- `tab_hash_write`: geomean 1.184183
- `alloc_tables`: geomean 1.000000
- `closures_upval`: geomean 1.311061

Command:

- `tools/ci/lua_test.sh m9_bench_stock_compare`

## Closure allocation churn

The older closure-allocation catastrophe is not reproducible in current state.
Focused `BENCH_SCALE=0.05` probes:

- Fork `closures_upval`: 60.24 ns/op.
- Stock `closures_upval`: 46.48 ns/op.
- Fork with `collectgarbage("stop")`: 55.48 ns/op.
- Stock with `collectgarbage("stop")`: 49.51 ns/op.

This leaves a real but much smaller gap. GC-side work is no longer the dominant
factor in this probe.

## x64 VM table/TNEW claims

The old "BC_TSETS always jumps to `vmeta_tsets`" claim is stale. Current
`src/vm_x64.dasc` has a bounded existing-slot `BC_TSETS_Z` fast path with
helper/CAS fallback where concurrent publication or GC barriers are required.

The x64 empty-table `BC_TNEW` entering-window fallback is already covered and
passed:

- `tools/ci/lua_test.sh m5_x64_tnew_empty_inline`

## `mt_entering` predicate audit

Remaining direct `mt_active_acq()` production uses are either paired with
`mt_entering_acq()` or are inside threading activation/shutdown machinery:

- `src/lib_os.c` rejects process-global locale mutation while active, live, or
  entering.
- `src/lj_gc.c` keeps pending-root single-producer fast paths disabled while
  active, entering, or GC workers exist.
- `src/lib_threading.c` owns the transition between `mt_entering`, `mt_active`,
  and `mt_live`.

Current entering-window coverage includes table clear/insert, legacy upvalue
snapshotting, explicit GC routing, os.setlocale rejection, JIT table-store
routing, and x64 empty `TNEW` fallback.

## Next useful work

The remaining high-value stability work is stress expansion rather than
repatching the claims above:

- Heavier table resize/retire stress around weak keys, finalizers, and traced
  reads/writes.
- JIT trace flush/side-trace stress under concurrent thread activation and
  shutdown.
- GC root publication stress across pending-root drains, active `mt_entering`,
  and GC2 worker activity.
