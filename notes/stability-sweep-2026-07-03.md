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
artifact reads remain available through `Test:read()` and
`suite_utils.read_file()`. Generated-output tests inspect artifacts produced by
the build/runtime, not repository source files.
This policy also covers the old milestone guard wrappers: they are historical
notes, not checks to reintroduce. If one described a real lockless invariant,
the invariant belongs in code comments/notes and in behavior, fixture, or
generated-artifact coverage when it is observable.

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

## Final cleanup pass

The temporary error-path and trace-exit diagnostics used while reducing the
flush race were removed. The assembly-time snapshot-PC owner scan was also
removed: it was a costly compiler-path rejection and the lasting lifetime rule
now lives in trace marking/retirement, where retired trace start and snapshot
prototype owners are preserved through the grace period.

Validation after the cleanup:

- clean assertion build: `make -C src -j$(nproc) CCDEBUG='-g -DLUA_USE_ASSERT'`
- focused JIT/flush suite:
  `tools/ci/lua_test.sh m5_jit_trace_publish m6_jit_mcode_publish
  m6_jit_flush_hs m6_jit_util_flush_race m6_jit_mt_activation_flush
  m6_jit_flush_thread_stress`
- five assertion-build heavy stress runs with
  `LJ_M6_JIT_FLUSH_THREAD_ROUNDS=128` and
  `LJ_M6_JIT_FLUSH_THREAD_CHURN=256`
- stock suite: `tools/ci/lua_test.sh run_stock_tests -- --quiet` (`509 passed`)

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

Standalone GC2 mark cycles no longer mutate legacy color bits. `GC2State` now
has a `legacy_mark_bridge` latch that is enabled only by legacy
`gc_mark_start()` after `lj_gc2_mark_begin()`. GC2 marks clear legacy white bits
only while that latch is set; true minor cycles and manual active-GC2 cycles
therefore keep arena liveness separate from legacy sweep colors. Sweep-boundary
root preservation still forces the preserved root legacy-live because that path
is explicitly protecting the legacy root-list close boundary. This fixed
paranoia failures where dead conservative stack hits such as temporary
`"dead..."` strings, and a stale standalone-cycle `"child"` string key, looked
legacy-live without corresponding GC2 marks.

GC2 weak-table discovery now owns an overflow node list for tables that do not
fit the bounded weak snapshot vector. Weak completion first drains the vector,
then clears overflow nodes and finally uses the legacy weak-list bridge as an
additional source. This preserves weak-value/all-weak semantics even when stale
legacy colors keep a reachable weak table out of `g->gc.weak`, and avoids
borrowing `GCtab.gclist` for a second GC2 list. Overflow metadata uses the raw
non-throwing allocator path so parked GC2 workers do not unwind through a
borrowed Lua stack on allocation failure.

The same paranoia harness also now covers the static no-JIT build path:
trace-scope retire actions are no-ops when `LJ_HASJIT=0`, matching the existing
trace flush/hasany macros instead of leaving an implicit declaration in
`lj_safepoint.c`.

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

Independent 50-process samples with the same coverage shape also stayed bounded:

- Fork: first run 17..36 traces, second run 21..40, second-run delta 3..4,
  up-recursion at trace 2..21, bad runs 0/50.
- Stock: first run 17..41 traces, second run 21..45, second-run delta 3..4,
  up-recursion at trace 2..26, bad runs 0/50.

Looped timing after one warm `fib(30)` still shows a current fork gap:

- Fork: about 7.26..7.45 ms per call across three 50-call process samples.
- Stock: about 5.33..5.60 ms per call across three 50-call process samples.

## JIT traversal stress

The table traversal stress exposed a `BC_KSTR` recorder assertion that predated
the KEYLOCK/nonblocking-`next()` change. Both current worktree and previous
pushed commit `4b2c26cb` aborted an assertion-build run of:

- `LJ_M5_TAB_RESIZE_STRESS_CASES=traversal`
- `LJ_M5_TAB_RESIZE_TRAVERSAL_MODES=next`
- `LJ_M5_TAB_RESIZE_STRESS_REPS=256`
- `LJ_M5_TAB_RESIZE_STRESS_TRAVERSAL_ROUNDS=64`

The assertion was in `lj_record_ins()` while recording `BC_KSTR`, checking that
a negative constant index names a string KGC entry. The bytecode and proto KGC
slot were valid; the referenced `GCstr` had been swept by the legacy string
intern-table path and its cell reused. GC2 proto traversal had marked the string
in arena bits, but the legacy string sweeper still uses the classic color byte.

`lj_gc2_markobj()` and the worker mark path now clear the legacy white bits for
all GC objects marked by GC2. The original fix only covered interned strings, but
the legacy root-list sweeper still uses the same classic color byte for traces,
prototypes, closures, tables, and other root-list objects. A GC2-only edge must
therefore publish liveness to both collectors until arena-only sweeping replaces
that root-list path.

Safepoint native acknowledgement was also tightened: remote native acks do not
consume `HS_SCAN_ROOTS` for TGs that own a Lua stack, and owner-side native-leave
scans walk frame headers before raw slots when the owner is in an interpreter
frame. If the owner TG is still in a trace/native helper (`jit_base` or positive
trace `vmstate`), both legacy GC and GC2 now preserve the whole stack storage and
do not decode `L->base` as an interpreter frame chain. The executing trace is
kept through `gc_traverse_curtrace()` and the per-TG trace root, which preserve
its prototype/IR graph without trusting a JIT-owned frame layout. GC2 also
preserves the assembler's unpublished `J->curfinal` trace copy as raw arena
memory during root scans; it is not a semantic trace root until `trace_save()`
publishes it.

Validation:

- exact `next`-only traversal reproducer above: passed under an assertion build
- `tools/ci/lua_test.sh m3_safepoint_handshake`: passed
- `tools/ci/lua_test.sh m3_gc_active_thread_roots`: passed
- focused `pairs` traversal and `ipairs` traversal with 512 resize rounds: passed
- repeated aggressive `next` traversal with `hotloop=1,hotexit=1`,
  `threads=4`, `reps=256`, and `rounds=64`: passed
- heavy `next` traversal with `threads=4`, `reps=1024`, and `rounds=256`: passed
- mixed `pairs,ipairs,next` traversal with `threads=4`, `reps=512`, and
  `rounds=128`: passed

The heavier assertion-build `next` traversal crash reduced to a self-applied GC2
root scan from a JIT/native table resize helper. The crashing frame walk came
from `tab_struct_owner_wait()` leaving native and applying `HS_SCAN_ROOTS` while
the TG still had trace state published. That path is covered by the JIT/native
stack-scan rule above.

## Stock runner stale table edge

`tools/ci/lua_test.sh run_stock_tests -- --quiet` exposed a stale table slot in
the generated stock-suite test plan. A reachable 4-slot table still held a
string-tagged TValue for `tostring/typeof semi-roundtrip`, but the arena cell had
already been reused by a `GCtrace`/cdata object. The reduced repro was:

- `cd tests/stock/test`
- `LUA_PATH=... src/luajit test.lua --quiet 75`

`-joff` passed, which pointed at JIT-side table publication rather than stock
semantics. The root cause was x64 JIT table stores using stock direct lowering
when MT was inactive, and also allowing GC-object stores into trace-local tables
that later escaped the trace. GC2 arena marking and weak/remembered barriers are
required even for ordinary single-thread JIT code whenever an incremental or
generational mark can overlap the trace. The fix routes published ASTORE/HSTORE
through the GC2-aware helpers regardless of MT activation and keeps the
trace-local direct path only for non-GC TValue stores. A sweep-boundary preserve
hook now also traces any root first discovered at SWEEP time so preserving a root
cell cannot leave its children reclaimable.

Verification after the fix:

- Assertion build: `test.lua --quiet 75`: passed
- Assertion build: `tools/ci/lua_test.sh run_stock_tests -- --quiet`: 509 passed
- Optimized build: `tools/ci/lua_test.sh run_stock_tests -- --quiet`: 509 passed
- `tools/ci/lua_test.sh m3_safepoint_handshake m3_gc_active_thread_roots m5_tab_resize_stress m6_jit_recursive_call_unroll`: passed
- Heavy table traversal stress with `threads=4`, `reps=1024`, `rounds=256`: passed
- Optimized threaded flush stress with `LJ_M6_JIT_FLUSH_THREAD_ROUNDS=128`
  and `LJ_M6_JIT_FLUSH_THREAD_CHURN=256`: passed

## Benchmark guard

The benchmark-regression coverage is no longer purely self-referential in current
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
  reads/writes. Follow-up coverage now adds the `weakfinjit` case to
  `t-tab-resize-stress.lua`, exposed as
  `m5_tab_resize_weakfinjit_stress`: weak-key table entries whose values are
  finalizable cdata, traced readers over rooted object keys, concurrent resize
  writers, and parked GC2 workers.
- JIT trace flush/side-trace stress under concurrent thread activation and
  shutdown. `tests/t-jit-flush-thread-stress.lua` is wired as
  `m6_jit_flush_thread_stress`. Follow-up coverage adds
  `m6_jit_flush_thread_heavy_stress` with 4 long-lived workers, 96 flush rounds,
  192 short-lived thread activations, and progress snapshots on join failure.
  The stress now reports recent worker/churn progress, live trace count, and GC2
  telemetry before surfacing a timeout/error.
- GC root publication stress across pending-root drains, active `mt_entering`,
  and GC2 worker activity.
