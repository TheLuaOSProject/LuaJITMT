# Stability sweep 2026-07-03

Scope: follow up on older catastrophic-regression claims against current
`v2.1` after the entering-window table/JIT fixes and invariant-guidance cleanup.
The first benchmark-only pass found several claims stale, but the later
flush/GC reducer exposed real stability bugs. This sweep now includes runtime
fixes for scoped trace retirement, stale x64 loop bytecode fallback, legacy
GC mark-cycle roots, table-generation reclamation, single-TG emergency trace
flush, and trace-number reuse under concurrent full/scoped flush.

## Repository Text Policy

The active Lua harness and CI no longer predicate pass/fail on repository
source text.
The old repository-source enumerator was removed from `tests/lib/ljtest.lua`;
artifact reads remain
available through `Test:read()` and `suite_utils.read_file()`. Current scans
only find documentation/historical notes or generated-output/source-path build
usage, not active repository-text tests.

## Scoped trace flush and stale loop fallback

Scoped flush now marks traces with `TRACE_SCOPE_FLUSH_PENDING` and treats that
bit as part of trace runnability. Side-trace publication uses the parent exit
target as the final runnable gate, and x64 trace exit keeps `jit_base` live
through snapshot restore so trace-flush handshakes do not reclaim mcode while a
TG is still unwinding.

x64 `BC_JLOOP` fallback recovers original `FORL`/`ITERL` loop offsets from the
matched or retired trace body only when `PC-4` is still the trace start PC. This
prevents a stale `JFORL`/`JITERL` bytecode from falling through after one
iteration, while avoiding the earlier wrong-PC branch that could recurse in the
VM. Single-TG recorder emergency full flushes use the direct flush path instead
of waiting for their own safepoint acknowledgement; public/full handshakes still
advance the safepoint epoch.

The VM now publishes `TG(jit_base)` before holding a trace body or mcode pointer
across the final entry checks, then revalidates the slot, retire epoch, pending
bit, and start PC before jumping. If the trace is no longer runnable, it clears
the entry publication and falls back to interpreter dispatch. Full/scoped flush
reserves retired trace slots with the existing pending sentinel through the
trace grace period, then releases the number while keeping the retired body
available for stale `startins` recovery until the legacy GC root link is gone.
Retired mcode areas now check live trace slots, retired bodies, and heap
exit-target tables before freeing. The final trace-quiescence wait treats
either non-null `jit_base` or positive TG `vmstate` as active trace state.

`tests/t-jit-flush-thread-stress.lua` is now wired into M6 as
`m6_jit_flush_thread_stress` to cover concurrent full flush, per-trace flush,
short-lived thread churn, and stale loop bytecode recovery as behavior.

Residual follow-up: a standalone unmonitored 100-iteration run of the stress
script has intermittently hit the outer timeout, while the M6 case and a
live-monitored 80-iteration run completed. Treat future timeouts as a diagnostic
target; capture live thread stacks before changing runtime behavior.

## Legacy GC bridge

Forced full GC no longer clears partial gray/weak state and jumps straight to
sweep. It finishes any active mark fixpoint first; otherwise a root table can
survive while its keys, values, or backing arrays are collected.

Retired traces are now treated as SMR-protected bodies, not semantic roots. The
retired-list marker preserves the trace body and exit table from physical free
without recursively marking stale IR constants or start prototypes. Legacy
mark-cycle start normalizes colors and forces primary/explicit roots into the
fresh frontier once, so SMR-preserved non-white bodies cannot cause reachable
tables or metatables to be skipped by the next cycle.

Table-generation reclamation now checks that a retired node/array is not still
the table-published root before freeing it. The check is bounded and cold-path
only; it preserves stock table semantics without adding a warm-path lock.

Close-time FFI callback owner disowning now tolerates a `lua_State` whose
`glref` has already been cleared during shutdown cleanup.

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
  shutdown. `tests/t-jit-flush-thread-stress.lua` remains a manual reducer for
  this: it still finds a GC propagation crash under small settings and is not
  wired into the M6 suite until that failure is fixed.
- GC root publication stress across pending-root drains, active `mt_entering`,
  and GC2 worker activity.
