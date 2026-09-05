# Initial SWEEP coalescing performance pilot

Date: 2026-09-05 UTC. Linux x64 only.

The initial coalescing runtime completes the full JIT harness with an
unweighted fork/stock geometric mean of **1.425195179** across 15 rows.
Insertion improves substantially relative to the prior leaf-publication pilot;
closure allocation, coroutine switching, and interpreter table operations
remain expensive. The interpreter run reaches its 180-second limit after six
rows, so it supplies no complete-suite aggregate.

These measurements predate the public MARK table-barrier admission and
saturation repair identified during independent review. They also exclude the
empty-reclaimed arena optimization. They preserve the measured costs that
guided the next work; final correctness and performance evidence must identify
the repaired source separately.

## Reproducible source and protocol

The frozen normal static runtime is
`/tmp/lj-gc-jit-combined-20260905-6cxpl6mp/normal`, based on `f8faf862` with
initial coalescing and the deferred JIT root-abort retirement overlaid. It
includes the previously committed leaf-publication and scalar arena-statistics
repairs. The JIT production overlay matches the subsequently committed
`1934afcd`. The executable SHA-256 is
`8b75419d1972794ab6287dc9be8a4aad7b8bdd14f581a0fb4a776e24602bbd75`.

[Metadata](../bench/gc-coalescing-pilot-2026-09-05/metadata.json) records every
production source hash, the original build command, the harness hash, and the
pinned stock revision `b925b3e3fc6771171602323b45fbe9fb8fc90369`.
[The runtime patch](../bench/gc-coalescing-pilot-2026-09-05/measured-runtime.patch)
preserves the production overlays relative to that base. Stock's normal
executable is unchanged from the earlier pilots, SHA-256
`d399449cc8cee4b0c600104a4a66fd44eeeac276c0f8571ce8204744041b5e34`.

Both runtimes execute the unchanged `plan/aux/bench/bench.lua` at
`BENCH_SCALE=1`, with GC mode unset and their own Lua module paths. The order
is fresh stock JIT, fork JIT, stock interpreter, fork interpreter. Each process
has a 180-second limit and line-buffered output. All are pinned to logical
CPU 30; other functional work runs on CPUs 0–15. The host and CPU frequency
are not exclusively controlled. Runtime builds have no assertions, test hooks,
sanitizer, or profiler instrumentation.

Each row reports the minimum CPU time from five rounds in one process, with
full collection before each round. This is one process per runtime/mode,
so small differences need repeated independent samples before driving tuning.
The geometric mean weights workloads equally; slow cases dominate actual
process wall time.

## Results and remaining costs

| JIT workload | Stock ns/op | Fork ns/op | Fork / stock |
| --- | ---: | ---: | ---: |
| New string-key insertion | 120.05 | 1,827.49 | 15.22 |
| Closure creation/upvalue mutation | 73.15 | 4,055.45 | 55.44 |
| Coroutine switching | 28.28 | 235.24 | 8.32 |
| Existing-key stores | 1.92 | 2.05 | 1.07 |
| Existing-key reads | 2.02 | 2.13 | 1.05 |

The prior leaf-publication full pilot reported insertion at 6,731.25 ns/op
and a 1.540550898 JIT geometric mean. The lower insertion cost in this
integrated pilot is consistent with removing repeated complete scans of
queued duplicates. The two historical pilots include additional source
changes and separate process schedules, so they do not isolate coalescing's
causal contribution.

The interpreter's completed rows include existing-key stores at **662.66
versus 13.05 ns/op**, new-key insertion at **3,767.87 versus 135.07 ns/op**,
and hash reads at **412.31 versus 46.37 ns/op**. The nine rows starting with
`tab_read_existing` are missing. Missing rows remain empty in the comparison
and never enter an aggregate.

| Process | Wall seconds | Rows | Outcome |
| --- | ---: | ---: | --- |
| Stock JIT | 7.988 | 15/15 | Exit 0 |
| Fork JIT | 117.669 | 15/15 | Exit 0 |
| Stock interpreter | 31.231 | 15/15 | Exit 0 |
| Fork interpreter | 180.005 | 6/15 | Timeout, exit -9 |

[Comparison CSV](../bench/gc-coalescing-pilot-2026-09-05/comparison.csv),
[process records](../bench/gc-coalescing-pilot-2026-09-05/runs.json), and
[aggregate status](../bench/gc-coalescing-pilot-2026-09-05/summary.json)
retain all expected workloads, commands, environments, exits, and missing
rows. Raw stdout/stderr and the original driver are in the same directory.
All four processes are terminal and post-run executable hashes match.

The separate
[closure diagnosis](closure-upvalue-performance-diagnosis-2026-09-04.md)
identifies repeated processing of retained empty arenas and scanning the
harness's live graph as major costs. That supports a narrowly certified arena
skip before any pacing change. Interpreter table-operation costs and the
whole-table traversal budget remain separate implementation work. These
measurements leave the performance and full nonblocking acceptance gates open.
