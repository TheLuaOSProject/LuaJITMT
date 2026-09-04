# Current performance review: stock versus exact fork revision

Date: 2026-09-04, UTC. This is review evidence, not final performance acceptance.

The complete, unprofiled JIT pilot exposed a 52.05x closure/upvalue slowdown,
13.52x new-key insertion slowdown, and 8.96x coroutine-switch slowdown. Its
15-workload geometric mean was 1.402104x stock. The interpreter pilot exposed a
much larger insertion cliff, approximately 1257x stock, and was deliberately
stopped after profiling identified repeated GC table traversal. These are
current measurements at the revisions below; the July 9.45x/8.39x release
figures are historical measurements with different sampling conditions.

## Artifacts and verification scope

- [Compact comparison CSV](../bench/runtime-performance-review-2026-09-04.csv)
  contains every JIT row, the reported interpreter rows, explicit missing
  interpreter results, and the measurement status.
- [Stock raw CSV](../bench/runtime-performance-review-2026-09-04/stock.csv) and
  [fork raw CSV](../bench/runtime-performance-review-2026-09-04/fork.csv) retain
  the harness totals and ns/op fields. The fork interpreter rows are labeled
  `profiled_partial_pilot`; missing rows are not invented or treated as passes.
- [Metadata](../bench/runtime-performance-review-2026-09-04/metadata.json),
  [process records](../bench/runtime-performance-review-2026-09-04/runs.jsonl),
  build logs, raw stdout and raw stderr are in the same artifact directory.
- [Termination record](../bench/runtime-performance-review-2026-09-04/interpreter-pilot-deliberate-termination.json)
  records the exact deliberate stop and the five unreported workloads.
- [Profile summary](../bench/runtime-performance-review-2026-09-04/pilot-fork-interp-perf-flat.txt)
  and [symbolized call stacks](../bench/runtime-performance-review-2026-09-04/pilot-fork-interp-perf-report.txt)
  retain the diagnostic evidence.
- [Small insertion scaling CSV](../bench/runtime-performance-review-2026-09-04/insertion-scaling.csv)
  and its raw outputs preserve the bounded follow-up diagnostic.

Both source trees were created by `git archive` into
`/tmp/lj-runtime-performance-review-2026-09-04/{stock,fork}`. No shared
workspace runtime source was changed or built for these measurements. All
benchmark processes have terminated. The raw DWARF perf capture remains local
at `/tmp/lj-runtime-performance-review-2026-09-04/pilot-fork-interp.perf.data`;
only its symbolized reports are checked in, not captured stack-memory bytes.
Checked-in CSVs use LF line endings and profile reports have trailing spaces
removed; the recorded values and call stacks are unchanged.

## Revisions, build and machine

| Item | Value |
| --- | --- |
| Fork | `a649f737d9841e1bf17f9102fb526d6bfb6c29e3` |
| Stock | `b925b3e3fc6771171602323b45fbe9fb8fc90369` |
| Compiler | GCC 14.2.0, Debian `14.2.0-19` |
| Common optimization flags | `-O2 -fomit-frame-pointer -mcx16` |
| C dialect equivalence | Fork defaults to `-std=gnu11`; rebuilding stock with explicit `CCSTD=-std=gnu11` produced a byte-identical executable to the stock pilot binary. |
| Build mode | Default Linux x64 binaries; no sanitizer, assertion or test-helper flags added. |
| CPU | Intel Core i9-14900K, logical CPU 30, core ID 46, no SMT sibling for CPU 30 |
| Affinity | Every benchmark child pinned with `taskset -c 30` |
| CPU policy | `performance` governor; reported maximum frequency 4.4 GHz for CPU 30; no cgroup CPU quota (`cpu.max` was `max 100000`) |
| Harness | The fork archive's unmodified `plan/aux/bench/bench.lua` |
| Harness SHA-256 | `ebd0b8d53b6e7a340c90c45ad33d9bdd47acbd5418890d593d6aae127ef926a9` |

The executed initial build command, in each archive, was:

```sh
make -C src -j4 CC=gcc 'CCOPT=-O2 -fomit-frame-pointer' CCOPT_x64=-mcx16
```

The stock dialect-equivalence rebuild added `CCSTD=-std=gnu11`. Both stock
binaries have SHA-256
`d399449cc8cee4b0c600104a4a66fd44eeeac276c0f8571ce8204744041b5e34`.
The fork binary has SHA-256
`8f96ada9f586a74f8ffc1989a1683e9adbd0bce4cf127798781d8b893c19a694`.
`ldd` confirmed each executable includes its own LuaJIT runtime rather than
loading a system `libluajit`.

CPU affinity is not system-wide CPU isolation. Other repository work was
concurrent, CPU frequency was not fixed, and the measurements are single
process pilots. These constraints matter for fine differences near parity;
they do not justify declaring a repeated-sample acceptance gate passed.

## Sampling and exact process outcomes

The full harness ran with `BENCH_SCALE=1`, no filter, `-jon` or `-joff`, and
`io.stdout:setvbuf('line')` before loading the unmodified script. `LUA_PATH` and
`LUA_CPATH` pointed at the corresponding archive's runtime modules;
`BENCH_GC_MODE` was unset. Exact argv, working directories, UTC timestamps and
resource accounting are in `runs.jsonl`.

The process order interleaved implementations: stock JIT, fork JIT, stock
interpreter, fork interpreter. Each reported workload is the harness's minimum
`os.clock()` elapsed CPU time over five iterations, with a full collection
before each iteration. The five internal iterations are not five independent
process samples. No additional whole-suite repetitions were run after the
pilot revealed the regressions and the review owner requested diagnostic
closure.

| Process | Wall seconds | User seconds | System seconds | Outcome |
| --- | ---: | ---: | ---: | --- |
| Stock JIT | 7.978760 | 7.927517 | 0.023917 | Exit 0, 15/15 rows, unprofiled |
| Fork JIT | 132.102458 | 128.835486 | 2.861594 | Exit 0, 15/15 rows, unprofiled |
| Stock interpreter | 31.940444 | 31.830385 | 0.022914 | Exit 0, 15/15 rows, unprofiled |
| Fork interpreter | 680.275095 | 675.802062 | 2.835368 | Deliberate SIGTERM, exit -15, 10/15 rows, profiled diagnostic |

The interpreter stop occurred at `2026-09-04T21:56:16Z`, after the review owner
requested ending the instrumented pilot and preserving its evidence. The
configured 1800-second timeout did not fire. The final raw output contains ten
completed rows: `alloc_tables` and `string_intern` completed after an earlier
eight-row progress observation and before termination. The unreported rows are
`closures_upval`, `upval_hot`, `ffi_struct`, `coroutine_switch`, and
`sbuf_format`. There was no runtime stderr in any of the four benchmark
processes.

A perf attachment attempt on the JIT process occurred after that process had
already exited; it failed before attaching and did not perturb the JIT timing.
The subsequent interpreter sample did attach, so the entire interpreter pilot
is labeled diagnostic even where a particular row preceded the sample.

## Observed regressions

Selected complete JIT results, in ns/op:

| Workload | Stock | Fork | Fork / stock |
| --- | ---: | ---: | ---: |
| `closures_upval` | 73.59 | 3830.60 | 52.053268 |
| `tab_insert_newkey` | 113.62 | 1536.39 | 13.522179 |
| `coroutine_switch` | 26.42 | 236.77 | 8.961771 |
| `fib30` | 6245000.00 | 8716000.00 | 1.395677 |
| `tab_store_existing` | 1.83 | 1.90 | 1.038251 |
| `ffi_struct` | 0.68 | 0.69 | 1.014706 |

The JIT geometric mean of all 15 reported ratios is 1.402104. The faster
`string_intern` and `sbuf_format` rows, at approximately 0.117x and 0.108x stock
respectively, materially lower that aggregate despite the large slow rows.
Neither these speedups nor tiny arithmetic differences were separately
diagnosed. Per-workload ceilings remain necessary alongside an aggregate gate.

Selected interpreter results from the profiled, deliberately incomplete pilot:

| Workload | Stock ns/op | Fork ns/op | Fork / stock |
| --- | ---: | ---: | ---: |
| `tab_insert_newkey` | 139.02 | 174761.75 | 1257.097900 |
| `alloc_tables` | 54.91 | 4341.34 | 79.062830 |
| `tab_read_existing` | 14.28 | 1019.07 | 71.363445 |
| `tab_store_existing` | 12.50 | 648.52 | 51.881600 |
| `tab_array` | 12.47 | 247.62 | 19.857257 |

These partial diagnostic values are evidence of a severe problem, not a
complete interpreter acceptance result. No interpreter geomean is reported.

## Profile and source-supported explanation

The interpreter was sampled for five seconds at 49 Hz using user-mode cycle
events and DWARF call graphs while it was executing the large insertion
workload. The capture contained 247 samples with zero lost samples. Relinking
the existing fork objects without stripping supplied symbols; its build ID
matched the measured executable, `1aa21404bbd9ee43680914324fd0275140d27b45`.
The measured executable was not relinked or replaced.

The dominant stack is:

```text
lj_BC_CAT / lj_meta_cat
  gc2_step_auto
    lj_gc2_step_explicit
      gc2_worker_drain_inner
        gc2_traverse_tab_rec
          gc2_trace_sweep_tv_edge
            exact object admission / retain / mark / recovery
```

Sampled self costs include `hugetab_search` 12.16%, `hugetab_reader_entry`
11.02%, `gc2_small_candidate_admit` 9.92%, counted-lease release 9.37%,
`lj_arena_rescue_enter` 6.93%, and `arena_publish_leave` 6.53%. This interval
was dominated by GC traversal and allocation identity/lifetime bookkeeping,
rather than the direct new-key publication instructions alone.

The source supplies a concrete amplification mechanism:

1. The common table-store miss in `meta_tset_rooted_mode()` publishes the table via
   `lj_gc_pubtab()` (`src/lj_meta.c`, near line 846 at the reviewed revision).
2. During SWEEP, the barrier reaches `gc2_sweep_barrier_obj()` and the public
   `gc2_trace_sweep_edge()` path. Its current-scan suppression is conditioned
   on `worker_edge`; public publication can enqueue the parent again.
3. `gc2_traverse_tab_rec()` (`src/lj_gc2.c`, near line 18547) scans the complete
   array and hash vectors. It has no persistent slot cursor or per-slot budget;
   a growing table remains one nominal worker unit despite its changing cost.
4. For each GC-valued edge, `gc2_trace_sweep_tv_edge()` first acquires an exact
   TValue lease, then calls the sweep tracer, whose
   `gc2_markobj_preserve_status()` path admits/retains the same candidate again
   while the first lease is held. Both admission chains appear in the profile.

Repeated parent scheduling combined with whole-vector traversal can amplify
the per-edge checks substantially during insertion. The source audit also
identified store-guard dirty/rescan handoff as another producer. This mechanism
is supported by the sampled call path; the sample does not establish the exact
number of repeated scans or prove an asymptotic complexity bound.

An optimization must preserve the existing lifetime, weak-table, resize and
close proofs. Candidate directions are durable bounded traversal cursors,
precisely justified per-edge publication that avoids redundant whole-parent
rescans, and consuming an already-held exact lease instead of independently
admitting the same object again. Removing a barrier or metadata check without
the replacement proof would trade this performance problem for a correctness
failure. The separate MARK-close scheduling fix under review does not by
itself repair this SWEEP traversal mechanism.

## Bounded insertion scaling diagnostic

Fresh stock/fork processes ran the same unmodified harness with filter
`tab_insert_newkey`, JIT disabled, CPU 30, and scales 0.005/0.01/0.02. These
produce 1000/2000/4000 insertions per internal iteration. Implementation order
alternated between adjacent sizes. Each process had a 20-second timeout; all
six exited 0, with empty stderr, and the complete diagnostic took under one
second. Each row still uses the internal best of five.

| Insertions | Stock ns/op | Fork ns/op | Fork / stock |
| --- | ---: | ---: | ---: |
| 1000 | 67.00 | 1044.00 | 15.582090 |
| 2000 | 68.50 | 1021.50 | 14.912409 |
| 4000 | 71.25 | 999.25 | 14.024561 |

At these small sizes the per-insertion cost is roughly flat. This diagnostic
does not demonstrate O(n^2) scaling. It shows that small filtered tests expose
approximately 1 microsecond per insertion while the large full-suite run
reported approximately 175 microseconds. The current evidence does not
separate table size, GC phase and preceding-workload history; a future focused
investigation should count phase-specific rescans and scanned slots rather
than assuming those effects are interchangeable.

## Consequences for acceptance

The current M9 stock comparator defaults to a 100x ceiling and selected filtered
JIT workloads (`tests/suites/m9_m10_gc.lua:526-555` and
`tests/lib/bench_driver.lua:147`). It would permit the measured JIT closure
cliff, and its `compare_bins()` invocation does not run the interpreter column.
A green result there cannot establish the goal's performance requirement.

Retain both filtered and representative full-sequence workloads, JIT and
interpreter modes, a long-enough allocation/GC regime, per-workload ceilings,
and the final <=1.10 geomean target. Add active-MT and GC-churn throughput and
progress measurements separately. This pilot is single-mutator Linux x64;
it does not establish Windows/macOS parity, active-MT performance, generic
CALLXS ABI throughput, or correctness under racy Lua programs.
