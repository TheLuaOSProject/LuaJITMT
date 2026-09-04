# SWEEP leaf publication performance review

Date: 2026-09-04. Linux x64 measurements; no Windows or macOS validation in
this review.

Requiring `gc2_gct_may_traverse(gct)` before the final SWEEP SSB publication
removes unnecessary queue/recovery work for leaf objects such as strings.
The prior condition used the allocation arena's traversable flag, which does
not prove that a particular object has child edges. This review measures that
single correction after retained-admission reuse and the public table/FINREG
stability fixes. It includes no table-request coalescing or GC statistics
changes.

## Frozen sources and machine

[Metadata](../bench/gc-sweep-leaf-performance-2026-09-04/metadata.json) records
source and binary hashes, build identity, loader output, CPU policy, and the
stock archive's prior build manifest. The two fork trees differ in exactly one
tracked runtime source file, with the complete difference retained in
`runtime-source-difference.patch`:

- Before: `09cef065`, including retained SWEEP admission and the public
  table/observational FINREG fixes. Frozen normal static build at
  `/tmp/lj-gc-sweep-lease-20260904-2p5yleu8/tree/`.
- After: the same code plus the leaf-type publication guard. Frozen normal
  static build at
  `/tmp/lj-sweep-coalesce-review-20260904-2cpu_bml/tree-leaf/`.
- Stock: pinned archive `b925b3e3fc6771171602323b45fbe9fb8fc90369` at
  `/tmp/lj-runtime-performance-review-2026-09-04/stock/`. Its executable hash
  remains `d399449cc8cee4b0c600104a4a66fd44eeeac276c0f8571ce8204744041b5e34`,
  matching the earlier performance review and its byte-identical explicit
  GNU11 rebuild.

The fork builds use normal GCC 14.2.0 optimization, with no assertion,
test-helper, sanitizer, or profiler instrumentation. Each measured child is
pinned to logical CPU 30 of an Intel Core i9-14900K. That CPU has no SMT sibling
and uses the `performance` governor. Frequency is not fixed and there is no
system-wide isolation. Other agents continued functional work, with a request
to keep it off CPU 30 where practical. These conditions qualify small
differences near parity.

## Seven fresh-process insertion pairs

The checked-in [plain Lua benchmark](../bench/gc-sweep-leaf-performance-2026-09-04/insertion.lua)
fully collects before creating its table, times insertion of 5,000 distinct
string keys with the interpreter, then fully collects and checks every value,
the total entry count, and the value sum. GC stays enabled throughout. Both
versions run exactly the same script and initialization. Each process produces
one timed sample; this is not a best-of-five loop within one process.

Odd-numbered pairs run before/after, and even-numbered pairs reverse that
order. All 14 processes exited zero before their 30-second limits, with empty
stderr and the expected 5,000 values after collection.

| Timed insertion result | Before | After |
| --- | ---: | ---: |
| Median ns/key | 401,201.4 | 2,475.6 |
| Fresh-process samples | 7 | 7 |

The geometric mean of paired after/before ratios is **0.006159525**,
approximately a **162-fold reduction for this reproducer**. Individual ratios
range from 0.006121175 to 0.006189579. These samples establish the removal of a
large regression in this workload; they do not establish stock performance
parity or a general runtime speedup.

[Raw CSV](../bench/gc-sweep-leaf-performance-2026-09-04/insertion.csv),
[process records](../bench/gc-sweep-leaf-performance-2026-09-04/insertion-runs.json),
and [summary](../bench/gc-sweep-leaf-performance-2026-09-04/insertion-summary.json)
preserve the loop CPU times, process wall/user/system times, exact argv and
environment, validation totals, and paired ratios. Each process's stdout and
stderr are retained separately in the same artifact directory.

## Full harness pilot protocol

The full pilot uses the unmodified `plan/aux/bench/bench.lua`, SHA-256
`ebd0b8d53b6e7a340c90c45ad33d9bdd47acbd5418890d593d6aae127ef926a9`, with
`BENCH_SCALE=1`, no filter, and `BENCH_GC_MODE` unset. Lua module paths select
the measured runtime's own tree. Output is line-buffered so a timeout retains
completed rows. The order is fresh stock JIT, corrected fork JIT, fresh stock
interpreter, corrected fork interpreter. Each process has a 180-second limit.

Every full-harness row is the minimum CPU time from five iterations within
one process, with a full collection before each iteration. These rows are
single-process pilots, not five independent samples. A partial process cannot
establish a complete-suite aggregate. The full process records and raw output
retain failures and missing rows; none are substituted with zero or silently
dropped from an aggregate.

The complete JIT pilot reports these largest remaining gaps against the fresh
stock process:

| Workload | Stock ns/op | Corrected fork ns/op | Fork / stock |
| --- | ---: | ---: | ---: |
| Closure creation and upvalue mutation | 73.50 | 4,381.87 | 59.62 |
| New string-key insertion | 113.81 | 6,731.25 | 59.14 |
| Coroutine switching | 24.47 | 235.27 | 9.61 |
| Recursive `fib30` | 6,422,000.00 | 8,853,000.00 | 1.38 |
| Hash writes with string construction | 25.53 | 32.17 | 1.26 |

The geometric mean over all 15 JIT rows is **1.540550898 times stock**. Faster
string interning and formatting rows materially lower this aggregate despite
the much slower closure, insertion, and coroutine rows. Other tiny differences
near parity need stronger isolation and repeated sampling before they support
a tuning decision.

The full JIT process took 149.1404 wall seconds for the fork and 8.0226 seconds
for stock; both exited zero with all 15 rows and empty stderr. This complete
pilot is distinct from the earlier fork revision's full-harness measurements:
the leaf correction was measured after the public SWEEP rescan correctness
fixes. The small reproducer's improvement does not erase the full-sequence
insertion gap or establish the cause of every remaining slowdown.

The fresh stock interpreter process completed all 15 rows in 32.0734 wall
seconds. The fork interpreter process reached its 180-second limit after four
reported rows and was killed by the measurement wrapper. Its stderr was empty.
The completed rows include existing-key stores at **663.94 ns/op versus
12.69 ns/op for stock**, a 52.32-fold gap. No insertion row completed before
the timeout, and the eleven remaining rows are explicitly missing. An
interpreter geometric mean is therefore not computed.

| Full pilot process | Wall seconds | Rows reported | Outcome |
| --- | ---: | ---: | --- |
| Stock JIT | 8.0226 | 15/15 | Exit 0 |
| Corrected fork JIT | 149.1404 | 15/15 | Exit 0 |
| Stock interpreter | 32.0734 | 15/15 | Exit 0 |
| Corrected fork interpreter | 180.0025 | 4/15 | Timeout, exit -9 |

[Full comparison CSV](../bench/gc-sweep-leaf-performance-2026-09-04/comparison.csv)
retains every expected workload and marks missing interpreter values.
[Process records](../bench/gc-sweep-leaf-performance-2026-09-04/full-runs.json),
[raw reported rows](../bench/gc-sweep-leaf-performance-2026-09-04/full-results.csv),
and [aggregate status](../bench/gc-sweep-leaf-performance-2026-09-04/full-summary.json)
provide the commands, process limits, CPU and wall accounting, terminal exit
status, and the exact scope of the computed JIT aggregate. All four processes
are terminal, and post-run executable hashes match the initial hashes.

The remaining performance work must cover closure/upvalue allocation, full
sequence insertion, coroutine switching, and the interpreter's existing-key
path. The present measurements identify those gaps without attributing every
gap to the same source path. They leave both the performance acceptance gate
and the larger fully nonblocking runtime objective open.
