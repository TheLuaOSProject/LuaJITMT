# Dense overflow authority: normal cost and memory records

These are the original isolated d680 baseline/dense-W measurements from
`/tmp/lj-dense-overflow-20260905-7tl6kcfk`, preserved without a new timing run.
The [study note](../../notes/gc-table-dense-overflow-prototype-2026-09-05.md)
and [functional package](../../notes/evidence/gc-table-dense-overflow-prototype-2026-09-05)
explain the protocol, source patches and validation boundaries.

`cost-results.json` contains 153 completed exit-0 processes: 21 barrier runs,
84 plan-harness runs and 48 retained-memory runs. Commands, working directories,
explicit environment overrides, stdout and stderr are retained per process.
`cost-summary.json` is the original summary. Packaging recomputed it from the
raw records and checked exact equality; no workload was executed again.
`source-snapshot.json` identifies the measured sources, archives, executables,
cost harness and unchanged `plan-bench.lua`. Full binaries remain in the frozen
temporary tree, with their hashes preserved in the functional package's
`final-validation.json`.

The baseline and candidate use normal static builds with `CCDEBUG=-g`, default
optimization and no runtime helpers/asserts. The cost fixture itself keeps its
semantic assertions. CPU 31 was reserved for these runs; root work used CPU 30
and functional work could use CPUs 0–15. The host and frequency were not fully
isolated. Raw pilot and failed setup records remain separately labeled and do
not enter the 153-process summary. The candidate accessor comment and later
arena/scalar/direct-hit/Huge-tail changes are absent from both measured trees.

Every public MARK barrier run performs 1,000,000 real scalar stores with a
worker drain every 256 stores. It requires exactly that many authority
increments, the final stored value and no pending/recovery work. A final full
collection reaches IDLE outside the timed loop. Dense promoted state is
installed before timing. Seven fresh groups alternate/reverse their order.

| Barrier workload | Median ns/store + barrier | Median paired cost |
| --- | ---: | ---: |
| Exact d680, ordinary | 119.131 | — |
| Dense, ordinary | 119.435 | +0.262% versus base |
| Dense, promoted | 192.827 | +61.569% versus dense ordinary |

Ordinary paired costs range from +0.128% to +1.015%; promoted costs range from
+60.279% to +61.607%. These measure the whole public barrier/queue workload,
not isolated atomic-instruction latency.

The unchanged plan harness uses `BENCH_SCALE=0.02` and its existing best of five
rounds in each fresh process, with automatic GC enabled and collection calls
unchanged. Seven alternating baseline/dense pairs cover every case and mode.

| Plan workload | Mode | Base ns/op | Dense ns/op | Median paired cost |
| --- | --- | ---: | ---: | ---: |
| Table allocation churn | Interpreter | 2262.67 | 2268.94 | +0.241% |
| Table allocation churn | JIT | 1.07 | 1.08 | +0.935% |
| New string-key insertion | Interpreter | 993.25 | 1006.00 | +1.329% |
| New string-key insertion | JIT | 106.25 | 107.25 | +0.946% |
| Closure/upvalue churn | Interpreter | 1574.92 | 1577.80 | +0.102% |
| Closure/upvalue churn | JIT | 1582.50 | 1585.61 | +0.108% |

Paired percentage medians are computed from corresponding process pairs, not
from the ratio of the two displayed medians. The JIT allocation case permits
allocation elimination and is near timing/reporting resolution. These small
differences on a shared host do not establish broad performance parity.

The memory runs retain 20,000 tables, inserted keys or closures, verify their
contents, collect while rooted, release the roots, collect twice, then close
the state. Three fresh pairs cover four cases, including promoted tables,
with JIT off and on. Promotion uses real stores and barriers after test-only
namespace compression. Settled snapshots are IDLE with zero recovery.
Mapping counters use admitted descriptors; all recorded metadata admissions
succeed. Current RSS/VmSize, `mallinfo2`, requested/usable sidecar bytes and GC
totals are sampled outside timing. The inherited `rusage` maximum is retained
as a diagnostic and is not a workload peak.

| 20,000 retained tables, JIT off, after full collection | Base | Dense |
| --- | ---: | ---: |
| Traversable mappings | 41 | 41 |
| Sidecar requested bytes | 2,686,976 | 5,373,952 |
| Sidecar usable bytes | 2,687,304 | 5,541,232 |
| malloc mmap count / bytes | 0 / 0 | 41 / 5,541,888 |
| Median current RSS | 9,260 KiB | 9,124 KiB |
| Median virtual size | 13,412 KiB | 16,200 KiB |
| RSS growth since state opened | 5,768 KiB | 5,744 KiB |

This glibc allocates each 128 KiB dense sidecar with a 135,168-byte mmap. Most
untouched wide pages remain nonresident; the lower ordinary RSS is not a
portable memory saving. Promoting all tables adds a median 2,356 KiB within
dense (2,364 KiB with JIT on), compared with 100 KiB for base barrier/scan work
(104 KiB with JIT on). After roots are released, d680 still retains those
small mappings and their wide pages. Dense JIT-off RSS in the promotion case
is then 11,228 KiB versus 9,052 KiB for base. State close releases the dense
sidecar mmaps. Later arena-empty reclamation is outside this study.

No measured workload allocates a traversable Huge wide proof; Huge entries are
raw vectors. These records do not quantify per-Huge proof allocation cost or
validate the later mapping-tail alternative. No full-runtime performance or
nonblocking claim follows from this isolated study.

To reanalyze the preserved data only, copy `cost-results.json` and
`summarize-cost.py` into a temporary directory and run the script there. The
original build/run scripts refer to the frozen temporary layout and may write
results: reconstruct into a new directory before any future experiment.
