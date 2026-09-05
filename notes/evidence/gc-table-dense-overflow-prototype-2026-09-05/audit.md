# Reserved dense overflow authority: isolated prototype, 2026-09-05

This isolated candidate removes the **32-bit table-dirty exhaustion cliff** by
promoting an admitted table to a persistent wider authority. Its ordinary
updates still use CAS64 and its VM/JIT inline entry geometry is unchanged.
Focused strict and Clang ASan controls pass. Normal costs in the measured
workloads are close to exact d680; promoted barriers cost more and the reserved
metadata is materially larger. This is evidence for an integration candidate,
not evidence that the complete collector is nonblocking or that the final
combined runtime meets its performance target.

The base is `d680421c4cb50b85437d88255bc89358c5e3a6b1`. No shared production or
note files were edited. The four changed files are `src/lj_arena.c`,
`src/lj_arena.h`, `src/lj_gc2.c`, and `src/lj_gc2.h` (the last adds two test-hook
IDs). `source-manifest.json` and `final-validation.json` identify the tested
and measured files and binaries. `dense-W.patch` is the tested patch;
`dense-W-candidate.patch` adds only the separately preserved
`accessor-comment.patch`, documenting the small-only union accessor contract.
That comment was not silently added to the frozen binaries.

## Protocol and storage

The inline stamp remains `{covered_cycle32, dirty32}` in one CAS64 word,
followed by the unchanged CAS64 token. An ordinary mutation increments dirty
and clears coverage. When the prior inline dirty reaches `UINT32_MAX-1`, the
mutation first invalidates a reserved W entry using CX16, then installs the
per-incarnation inline sentinel `UINT32_MAX` with CAS64. An already promoted
mutation only invalidates W. W contains `{era64, covered_cycle32, serial32}`.
At serial MAX it advances era and returns serial to 1 in the same CAS128 that
clears coverage. The helper uses explicit CAS128 snapshots, not overlapping
mixed-width proof loads.

A scanner captures the domain and its proof before reading payload. An inline
scan never switches domains at completion. A wide scan must match both era
and serial and still see the inline sentinel before publishing coverage. This
rejects both an old inline scan whose low serial happens to equal the promoted
serial and an old wide scan across a low-word rollover. Existing exact token
generation, phase admission, recovery ownership, and proof-before-token
completion remain in place.

Both promotion contenders invalidate W before trying the mode CAS. A suspended
contender owns no shared INIT state: another contender can invalidate, publish
the mode and complete its request. If a later scan covers the second
contender's proof, that proof also follows the first contender's earlier
payload store and W invalidation. The loser can use that scan after seeing the
sentinel. There is no reset or initialization after publishing wide mode.

W is monotone for the entire mapping/cell lifetime, including reuse through
C allocation, VM TNEW, and emitted FNEW. Those existing private construction
paths reset only the inline state. Thus a reused cell can retain old W
coverage without using it; its later promotion invalidates W before exposing
the sentinel. Prior exact body readers must already be gone before private
incarnation reuse, independently of this authority protocol.

Small traversable mappings reserve a dense W array together with the existing
inline array before publication. Huge traversable mappings reserve one aligned
W allocation before publication. Reservation failure returns a private mapping
failure; no payload store has happened. Dirty invalidation never allocates, so
there is no new OOM branch after a committed store. Missing W on a promoted
published identity is an invariant violation and pins reclamation; it is not a
lazy-allocation fallback or a claim of indefinite progress.

The existing header pointer is an explicit immutable-mapping-kind union:
small mappings own the complete array, Huge mappings own one W, and plain
mappings leave it NULL. A W lookup requires a retained exact mapping and
readable allocation/body admission. The fixed-header token accessors remain
separate and W-blind. Side storage is freed only with the existing final small
unmap or either final Huge-unmap path. Transfer does not reset or relocate W.
The base/header/body offsets and the four emitted SHL4 indexes remain valid.
The final candidate's accessor comment explicitly requires non-Huge authority
before following `lj_arena_gc2_tabstamp_acq`.

| Compiled layout | Base | Dense W |
| --- | ---: | ---: |
| Inline entry including token | 16 B | 16 B |
| Token offset | 8 B | 8 B |
| Small sidecar allocation request | 65,536 B | 131,072 B |
| W array offset | — | 65,536 B |
| Huge fixed header | 128 B | 128 B |
| Huge inline stamp offset | 104 B | 104 B |
| First usable small cell | 616 | 616 |
| Separate traversable Huge W request | — | 16 B |

The wider namespace remains finite. At full era MAX / serial MAX, the sticky
universe reclamation veto and unsuppressible semantic publication behavior
remain. As in the base, the legacy scanner may publish a terminal coverage
stamp for queue/graph convergence, while semantic coalescing rejects that
terminal authority. This is a separate containment limit, not a solution to
full namespace exhaustion. Global cycle and token-generation limits also
remain unchanged.

## Correctness evidence

`strict-results.json`, `asan-results.json`, and the final consistent-FNEW
results contain exact commands, process exits, durations and output. Builds
and functional runs used CPUs 0–15. Runtime ASan used
`ASAN_OPTIONS=detect_leaks=1:abort_on_error=1` without suppressions. Only the
ASan build-time generators used `detect_leaks=0`.

- Eight real paused old scans cover inline/wide × small/Huge × legacy/exact
  token modes. A real payload store/public barrier follows the compressed
  namespace transition. Old exact scans leave the exact PENDING generation;
  a fresh scan completes it and marks the actual descendant graph.
- Four mode pauses cover both mapping kinds immediately before and after the
  sentinel CAS. A peer publisher completes while the first remains suspended.
  Previously current W coverage is already invalid before sentinel exposure.
- Twelve forced low-word rollovers and full collections on the same small
  table, and twelve on the same Huge table, all reach IDLE, drain recovery,
  retain a fresh descendant graph and clear an unreachable weak value without
  pinning reclamation.
- Existing SWEEP coalescing controls pass with their direct saturation
  injection moved to the full wide terminal pair. Full traversal forces the
  ordinary exact-token tests into W mode; legacy/exact proof, retry, phase,
  DESTRUCT, weak, retired-TG transfer and terminal controls pass. Recovery and
  table-store-guard fixtures also pass.
- Small FREE completion runs while its W page is PROT_NONE. Huge DEFER_FREE
  completion in MARK, WEAK and SWEEP runs with a PROT_NONE W pointer and a
  poisoned payload type byte. The header-only exact tokens complete without
  following W or touching payload. Leak-enabled ASan covers final frees.
- Actual VM TNEW and emitted FNEW at cells 1536/1537 preserve nearby guards and
  exact NONE generations, reject PENDING ownership, clear only the intended
  inline stamps and retain previous W authority. TNEW then performs a new
  promotion and proves that old W coverage is invalidated. VM/JIT production
  source hashes match exact d680; no width/stride changes are hidden.
- Reservation failure keeps small and Huge mappings private; a plain mapping
  still allocates. With calloc denied after the payload store, promotion makes
  zero allocation calls and the graph drains normally.
- The fresh normal candidate passes stock 387 `-joff` / 509 `-jon` tests.

Three deliberately broken **production** variants fail deterministically:
allowing an old inline completion to adopt W, ignoring era in wide completion,
and exposing the sentinel before W invalidation. The first two incorrectly
complete an exact old scan; the third exposes old current-cycle W coverage.
They are archived in `negative-results.json` with patches and build commands.

The inherited full FNEW setup first failed its SSB capacity assumption on both
the base and dense candidate. An explicit real buffer flush/root-snapshot
scheduling control passed, but it is **diagnostic only**: another inherited
setup falsified `allocf_arena` while retaining the arena allocator, leaving
CONSTRUCT/LINKING lanes unfinished. Passing that sequence is not valid
protocol evidence. The final `t-dense-fnew-consistent.c` uses a real
`mt_entering` eligibility failure, witnesses the VM slow helper, keeps the
semantic/SSB/root assertions, and requires no unfinished constructors after
each setup. It passes strict and ASan. The original false-allocator control
with the added invariant assertion fails in both. This valid fallback fixture
does not directly test a forged allocator-identity gate. See
`fnew-consistent-results.json` and `fnew-consistent-setup.patch`.

Development failures were kept, including the initial sidecar-size assertion,
an adapter warning, the original FNEW/capacity failures, a wrong stock working
directory, and the cost harness's initial assumption that MARK setup left the
seeded dirty counter unchanged. The corrected cost harness captures its start
counter after real MARK setup and requires exactly N subsequent increments.
No failing run is included in the final performance summaries. Intermediate
edit scripts are history; reproduce from the final patch and hashed fixtures.

## Normal cost and measured memory

The study built a fresh exact d680 baseline and dense candidate with identical
normal static flags (`CCDEBUG=-g`, default optimization, no helpers/asserts).
There were 153 completed fresh processes on reserved CPU 31. Root work ran on
CPU 30 and other functional work could run on CPUs 0–15; the host and frequency
were not fully isolated. Raw commands, environments, stdout and all samples
are in `cost-results.json`; `cost-summary.json` contains medians and ranges.
No profiler or GC suppression was used in these normal cost/memory samples.

Seven alternating fresh groups measure actual public MARK table barriers after
scalar payload stores, including a worker drain every 256 stores. Each process
performs 1,000,000 stores, requires exactly 1,000,000 authority increments,
checks the final value and zero pending/recovery work, then fully collects to
IDLE outside the timed loop. Promoted state is installed only in the dense
case before setup, outside timing. The group order reverses every other pair.

| Barrier case | Median ns/store+barrier | Median paired cost |
| --- | ---: | ---: |
| Exact d680, ordinary | 119.131 | — |
| Dense, ordinary | 119.435 | +0.262% vs base |
| Dense, promoted | 192.827 | +61.569% vs dense ordinary |

The ordinary paired range is +0.128% to +1.015%; the promoted range is +60.279%
to +61.607%. These are costs of this public barrier/queue workload, not isolated
instruction latencies or end-to-end application throughput.

The unchanged plan harness uses `BENCH_SCALE=0.02` and its existing best-of-five
rounds per fresh process. There are seven alternating base/dense pairs for
each case and execution mode. Automatic GC is enabled and harness collections
are unchanged.

| Plan workload | Mode | Base ns/op | Dense ns/op | Median paired cost |
| --- | --- | ---: | ---: | ---: |
| Table allocation churn | Interpreter | 2262.67 | 2268.94 | +0.241% |
| Table allocation churn | JIT | 1.07 | 1.08 | +0.935% |
| New string-key insertion | Interpreter | 993.25 | 1006.00 | +1.329% |
| New string-key insertion | JIT | 106.25 | 107.25 | +0.946% |
| Closure/upvalue churn | Interpreter | 1574.92 | 1577.80 | +0.102% |
| Closure/upvalue churn | JIT | 1582.50 | 1585.61 | +0.108% |

The JIT allocation-churn body permits allocation elimination and its result is
near timing/reporting resolution. It is not a measurement of materialized
table construction. The separate retained-object workload below requires the
objects to remain reachable and checks their contents. Differences this small
on the shared host do not establish general parity or a broad speedup.

Memory samples use 20,000 retained tables, 20,000 inserted keys, or 20,000
retained closures. They check completed values, retain the graph through a full
collection, release it, then perform two further collections and finally
close the state. Three fresh alternating pairs cover each case with JIT off
and on. The table-promotion case additionally does a real payload store and
public barrier for every retained table; base performs ordinary barriers,
dense compresses the inline namespace and performs real promotion. All
settled snapshots are IDLE with zero recovery.

Counters are collected outside timing. Small/Huge directory entries receive
counted mapping admissions before following side storage or asking
`malloc_usable_size`; all snapshots recorded zero denied admissions. The study
records `/proc/self/status` current RSS/VmSize, `mallinfo2`, exact metadata
request/usable bytes and GC totals. `rusage_maxrss_inherited_kb` proved to
include pre-exec inherited usage and is not used as a workload peak. Kernel
high-water diagnostics are retained but no precise unsampled peak is claimed.

For 20,000 tables with JIT off, both builds retain 41 traversable mappings:

| After full collection with tables retained | Exact d680 | Dense W |
| --- | ---: | ---: |
| Sidecar requested bytes | 2,686,976 | 5,373,952 |
| Sidecar usable bytes | 2,687,304 | 5,541,232 |
| malloc mmap count / bytes | 0 / 0 | 41 / 5,541,888 |
| Median current RSS | 9,260 KiB | 9,124 KiB |
| Median virtual size | 13,412 KiB | 16,200 KiB |
| RSS growth since opened | 5,768 KiB | 5,744 KiB |

On this glibc, each dense 128 KiB calloc uses a 135,168-byte mmap. The untouched
W half is reserved but mostly not resident. That allocation strategy explains
why the large metadata request increase does not appear as ordinary RSS
increase. It is not a portable memory saving. The requested sidecar bytes also
remain outside the runtime's ordinary GC live-byte accounting.

Promoting all 20,000 tables increases current RSS by a median 2,356 KiB inside
the dense process (2,364 KiB with JIT on); the corresponding base barrier/scan
work adds 100 KiB (104 KiB with JIT on). After roots are released and collected,
the d680 allocator still retains those 41/42 small mappings and their W pages.
Dense JIT-off current RSS is then 11,228 KiB in this promotion case versus
9,052 KiB for base. State close releases all sidecar mmaps. Base heap sidecars
may remain in the libc heap after close. The later arena-empty optimization is
not part of these trees, so its final retained-capacity behavior still needs
integration validation.

Insertion retains 18 traversable mappings and closures 35 (sometimes one more
after JIT flushing); the same twofold sidecar request and libc strategy recur.
These ordinary workloads create Huge raw vectors but no traversable Huge W
allocations. They therefore do **not** measure the separate per-Huge W
calloc/free cost or justify a mapping-tail optimization. That remains a
separate audited design/cost question.

The dense candidate is a reasonable bounded integration candidate given the
small measured ordinary-path cost and its unchanged emitted geometry. Its
upfront reservation, larger virtual/retained metadata and promoted cost must
remain explicit tradeoffs. Apply it onto the final arena/scalar/direct-hit
changes only with combined lifetime, emitted-path, allocator-failure and
workload validation. No release or broad nonblocking/performance claim follows
from this isolated study.
