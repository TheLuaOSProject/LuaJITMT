# Plain storage readers during a paused writer

This review starts at `c2ad32edafe7d8d65f398a0e5be62f7e4204f319` and keeps
Linux stability and exact storage lifetime ahead of performance. The preceding
change reclaimed explicit plain late frees, but its arena-wide writer gate
still rejected readers of unrelated live allocations. A deterministic real
storage-lease test fails against that commit while a foreign free is paused.
The public buffer pin loop retries this refusal, so a stopped writer can delay
an ordinary buffer read even though it modifies different storage.

The writer now closes the admission generation, publishes its exact target as
`FREEING`, and commits with a second exact CAS. A reader admitted before target
publication prevents that commit for as long as it can access the bytes. A
reader admitted after commit observes the target marker through the admission
word and rejects that allocation. Existing leases already check allocation,
late and sweep state before granting body access. They can now retain unrelated
live bodies during tentative, committed and completed ordinary writers.
No semantic GC classification or raw-reader hot-path check is broadened.

Intrusive remote-free queue targets already have their marker before queue
publication. A plain late drain instead snapshots its target bitmap between
tentative close and commit. A free published after commit cannot join that
snapshot: its reader might have entered earlier in the same generation. That
body and its wake remain available for a later drain. Lifecycle sweep,
quarantine, terminal close and generic committed `SEALED` owners retain their
stronger refusal. Allocation reuse remains vetoed until the counted generation
opens; widening observation does not permit premature reuse.

The review also found that `FREEING` plus an arena writer did not identify the
allocation whose semantic destructor retained that writer. A deferred free of
another `FREEING` allocation could consume the unrelated writer. A small plain
destructor now publishes its exact address in a header union whose other arm is
used only by huge mappings. Its owner atomically consumes that token when it
finishes. Duplicate late intent cannot revoke the token. Header size, arena
geometry and bin classes are unchanged.

Bin search previously stopped the entire chain at a gated plain run. The new
search can follow a validated owner-private free node without reusing it. The
node must have free bitmap state, no side owner, a valid extent and no lifecycle
transition. Foreign writers mutate allocated targets and remote queue nodes;
they do not own those free-bin links. The regression constructs a real same-bin
chain from two arenas, pauses a foreign writer in the first, and requires the
second arena's exact run to be reused before the writer resumes. Both pause
stages fail on the baseline by allocating a third arena.

Small in-place shrink protects the original allocation start, because a reader
of its header could otherwise retain bytes in the suffix being returned to a
bin. Shrink restores the previous sweep state after suffix publication, or
before leaving every aborted attempt. An earlier reader forces the existing
move fallback and keeps the old full extent intact. A reader of the same
allocation can still retry while an in-place shrink is paused. The source audit
found no active production small-shrink caller: the buffer and ordinary stack
shrink functions currently have no callers, and overflow stack relimiting uses
huge storage. This reachability result does not eliminate the generic allocator
progress obligation or its existing potential allocation failure when a shrink
moves. Those remain work before a full lock-freedom claim.

The new `m2_arena_plain_progress` gate combines the bin witness and a registered
runtime fixture. It checks observational and marking leases, bounded directory
admission, exact target rejection, a reader spanning writer completion,
destructor identity and duplicate late intent, a new late free after a drain
commits, retained-prefix shrink, and public `string.buffer` reads, writes and
free/refill before an unrelated writer resumes. The older directory test now
uses explicit lifecycle preparation to check refusal; a tentative ordinary
close is deliberately reader-admissible. Existing raw reuse and complete
allocator tests remain part of validation.

GCC release, Clang assertions, ASan, TSan, and builds without FFI, JIT or buffers
pass their applicable checks. Each release/assertion/ASan CI run passes 509
stock JIT source cases, 387 interpreter cases and 509 independently produced
stock-bytecode cases, together with the shared JIT, FFI and GC workloads. All
twelve standalone arena fixtures pass under assertions, ASan and TSan. The new
reader fixture and previous plain-late fixture pass in all three configurations;
assertions and ASan also pass complete GC traversal. ASan leak detection is
disabled; TSan does not instrument generated JIT machine code. The initial
assertion trial's stale directory expectation is preserved beside the successful
final checks.

The paired performance control uses the same frozen workloads as the preceding
review: three alternating process pairs per policy on CPU 8, each performing
800,000 checked 1 KiB reset/put/get cycles with JIT enabled. These are medians of
process means; explicit full-collection CPU time is recorded separately.

| GC policy | Previous ns/cycle | Candidate ns/cycle | Candidate / previous |
| --- | ---: | ---: | ---: |
| Automatic | 555.0 | 553.6 | 0.998 |
| Full collection every 20k | 569.1 | 591.8 | 1.040 |
| Full collection every 100k | 585.1 | 588.8 | 1.006 |

The every-20k policy's paired ratios range from 0.976 to 1.049. That measured
slowdown remains visible rather than being averaged into the automatic result.
RSS remains about 5 MiB. Both separate censuses end at five plain arenas and 19
late starts after 200,000 cycles; explicit free leaves one, and the first full
collection clears it. Five subsequent snapshots remain at zero. The live-heap
control retains 960-byte allocations across 16, 128, 1,024 and 4,096 arenas; its
median paired ratios are 1.036, 0.984, 0.996 and 0.989. One large baseline
wall-time outlier is retained and is not evidence of a candidate speedup.
All 60 measurement processes pass. These single-mutator controls do not
establish stock-performance parity or a multiworker memory bound.

The [performance report](evidence/plain-reader-progress-2026-11-26/performance/REPORT.txt)
preserves every row and input hash. The
[verification record](evidence/plain-reader-progress-2026-11-26/review/final-verification.json)
checks 82 direct runtime invocations across seven builds, all frozen inputs
against the final workspace, and the canonical runner's restored default build.
Composite CI invocations contain further cases. The
[independent source review](evidence/plain-reader-progress-2026-11-26/review/final/review.txt)
records the publication ordering and ownership assumptions.

This is an incremental progress correction, not release readiness. Active-worker
physical-memory bounds, ownership transfer and repeated detach still need
coverage. Windows and macOS remain deferred until a release candidate is ready.
