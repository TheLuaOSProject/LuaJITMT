# Complete Linux benchmark after positive rooted reads

The normal runtime with the arena reuse, scalar read, and broader positive-hit
changes completes all 15 full-harness rows in both modes. The fork/stock
geometric ratios are **1.337883709 with JIT** and **9.694639005 in the
interpreter**. These exploratory samples identify substantial remaining cost;
they do not establish performance parity or release readiness.

## Inputs and separate bounds

The fork executable is the previously frozen positive-hit normal static build,
SHA-256 `cfd0a47e14be6de5f1a3fde057abb88752ee7aeb516990b2884ae77acf01db17`.
Its production bodies match the change landed in `28de50a6`. The final header
documentation correction is separately preserved in the positive-hit evidence;
the receiver-tag optimization and FNEW fixture repair are absent here.
The pinned stock revision is `b925b3e3fc6771171602323b45fbe9fb8fc90369`,
executable SHA-256
`d399449cc8cee4b0c600104a4a66fd44eeeac276c0f8571ce8204744041b5e34`.

Both execute the unchanged `plan/aux/bench/bench.lua`, SHA-256
`ebd0b8d53b6e7a340c90c45ad33d9bdd47acbd5418890d593d6aae127ef926a9`,
at scale 1 with GC enabled and CPU 30 pinned. Each row selects the minimum of
five internal rounds. There is one complete fresh process per runtime/mode,
not repeated independent pairs. Other work used CPUs 0–15 and 31; this is a
shared host without frequency or memory-system isolation.

The original pilot runs stock JIT, fork JIT, stock interpreter, then fork
interpreter, with a 360-second bound on each process. Both JIT processes and
stock interpreter complete. Fork interpreter reaches 12 rows and times out;
its original summary retains a null aggregate and the timeout is preserved.
A **separate fresh fork interpreter process** runs the entire harness with a
900-second bound and completes in 727.427 seconds, exit 0. Its executable
hash is unchanged before and after execution. The complete interpreter
comparison pairs those 15 rows with the earlier stock interpreter sample,
which completed in 31.967 seconds. No partial row selection or scaled subset
is substituted. The longer bound itself is not evidence of a speedup.

## Remaining costs

| Full-harness case | JIT fork/stock | Interpreter fork/stock |
| --- | ---: | ---: |
| Existing-key stores | 1.02 | 21.59 |
| New-key insertion | 15.13 | 28.16 |
| Existing-key reads | 1.12 | 42.33 |
| Table allocation | 1.01 | 37.40 |
| Closure allocation | 21.61 | 20.22 |
| FFI struct access | 1.00 | 20.59 |
| Coroutine switching | 9.83 | 71.34 |
| String-buffer formatting | 0.11 | 70.87 |

Near-one-nanosecond JIT allocation/FFI rows can be optimized or virtualized by
the harness traces; they do not prove physical allocation or general FFI
parity. Large differences between cases make the geometric aggregate alone
an inadequate acceptance gate. The filtered matched positive-hit measurements
remain the evidence for that change's benefit. These complete rows instead
locate remaining generic operation, allocation, and call-publication costs.

The interpreter FFI row consumes 47.6172 CPU seconds per internal round at
1,587.24 ns/iteration. Independent smaller, GC-enabled filtered runs show
approximately linear cost. They attribute most cycles to generic metamethod
lifetime admission, with an additional small cost from the new table attempt
on a non-table receiver. The completed full run is not a nonconvergence
failure, and the profile does not justify removing the lifetime checks.

Exact source/build metadata, all original pilot results and raw output, the
separate extended command/environment/result, complete comparisons, and a
hash manifest are in `bench/linux-rooted-hit-full-performance-2026-09-05/`.
The original `performance/summary.json` is intentionally unchanged; use
`complete-summary.json` for the separately qualified complete observation.
