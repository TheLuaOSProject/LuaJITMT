# Rebuild free bins around the exact private bump window

Baseline: `4423c15f24688cbeafcc498e2cb9370b21cea950`. The preceding change
preserves long bin chains and removes quadratic insertion scans. This review
addresses a separate source of lost reusable capacity during bin rebuild.

An allocator can consume a prefix of an interior free run and keep the rest
as its private bump window. Other free runs can lie after that window. The old
rebuild callback used the bump cursor as the end of the entire arena scan,
discarding every later free run. An exhausted interior bump also excluded
later storage, despite having no private cells left.

This affects production ownership transfer: `lj_arena_alloc_transfer` moves a
quiescent source's arenas and rebuilds the destination bins while retaining the
destination's existing bump. The destination can therefore lose its own free
capacity when a dead worker's allocator is transferred to it.

The callback now uses exact interval exclusion. For every scanner-reported
free span, it publishes only the cells outside the active private `[cell, end)`
window, preserving both nonempty pieces when the span crosses it. The scanner
can coalesce adjacent free starts through the unpublished zero-state window.
It must not expose those private cells through a bin, alter the bump cursor,
or discard independent free storage. An absent or exhausted bump excludes
nothing. Adoption and sweep restoration have no private window in the arena
being staged and must continue rebuilding its complete eligible capacity.

Existing full-span ownership checks and final node-publication checks remain
required for every rebuilt piece. This change does not grant access through
a root, recovery, destructor, GC token or nonterminal lifetime owner. It adds
no peer wait and no new allocation fast-path synchronization. Full rebuild
continues to scan each owned arena; bounding that global work and dead-worker
retention are separate obligations.

## Deterministic regression

`tests/t-arena-rebuild-bump.c` uses real allocation, free, rebuild and transfer
operations. An allocation ledger independently identifies live cells, private
cells and all remaining reusable cells. Each published bin must cover its
cells exactly once, with no overlap into live or private storage. Rebuild must
preserve the bump cursor, every private byte and its metadata, and live payload,
lifetime, root and recovery state. Protected live guards bound the free runs.
There are no direct cursor edits or fabricated free-bin links.

Each case runs for PLAIN and TRAVERSABLE storage. Interior, exhausted-interior
and crossing-window cases repeat rebuild three times before exact reuse. The
crossing case first verifies that one real scanner callback spans both private
edges; it also includes independent earlier and later free runs. The original
tail case is a control. Transfer moves a distinct quiescent source's arenas,
preserves the destination bump and checks complete source-tail publication.

| Case, for both kinds | Frozen baseline `4423c15f2` | Candidate |
| --- | --- | --- |
| Interior window | 0/64 free cells; suffix missed; 1 to 2 arenas | 64/64; exact suffix reused; 1 arena |
| Exhausted interior window | 0/64 free cells; suffix missed; 1 to 2 arenas | 64/64; exact suffix reused; 1 arena |
| One span crossing both edges | 96/224 free cells; only earlier 1 KiB run reused; 1 to 2 arenas | 224/224; both complements and all distinct runs reused; 1 arena |
| Original arena tail | Pass | Pass |
| Allocator transfer | 3,479/3,543 free cells; destination suffix missed; 2 to 3 arenas | 3,543/3,543; destination suffix and source capacity reused; 2 arenas |

The eight baseline regressions return ordinary exit 1 with complete diagnostics
and empty stderr. The two original-tail controls pass. An initial candidate
smoke exposed an overly strict test assertion that an exhausted bump must have
a null arena pointer. The runtime may retain its pointer with `cell == end`;
the test was corrected to require an empty interval. That initial failure is
preserved separately. No runtime change was needed for it.

The source review confirms public reachability through `thread:join()` followed
by dead-thread allocator transfer when its existing quiescence checks permit
it. The exact layout regression uses allocator helpers; it is not presented
as a deterministic Lua-only reproducer. The existing public worker tests
provide separate integration coverage.

This change repairs capacity discovery in rebuild. It makes no throughput or
stock-parity claim. The allocation/free hot paths are unchanged, and a scanner
callback emits at most two fragments. A full owner rebuild still scans all
owned arenas. The prior long-bin insertion/search improvements, physical
capacity retained by live shrinks, huge-shrink failure path and worker-exit
metadata retention remain separate concerns.

## Validation and evidence

All seven Linux configurations build: GCC release, Clang assertions, Clang
ASan, GCC TSan, no FFI, no JIT and no buffer. The main validation matrix has
91 passing runtime invocations: 8 GCC, 28 assertions, 28 ASan, 22 TSan,
2 no-FFI, 2 no-JIT and 1 no-buffer. Each stock/CI matrix counts as one process
in that total. GCC, assertions and ASan each pass 509 stock source cases,
387 eligible interpreter cases and 509 independently produced stock-bytecode
cases, along with the shared Linux CI workload. The allocator matrix includes
all 14 standalone fixtures, retained-shrink and reader-progress tests, and
the existing side-owner, sweep, quarantine and adoption regressions.

All 12 canonical gates pass, including the new `m2_arena_rebuild_bump` gate and
explicit sweep coverage. Public buffer and GC-worker/trace-flush workloads,
storage leases and GC traversal remain covered. ASan leak detection is
disabled; TSan does not instrument generated JIT code. Windows and macOS
validation remain deferred until release preparation.

Each immutable build records 1,237 source inputs, including 228 runtime inputs.
The [final verification](evidence/arena-rebuild-bump-2026-11-26/review/final-verification.json)
checks every declared source against the final workspace, all produced binaries
and the restored normal workspace build. Frozen runtime source SHA256 is
`471b215c6e54082b6c27c17bb3fb155e4baffc6de819842bbc578e979048dfbf`;
the frozen GCC library SHA256 is
`c880ce0ddee055eb92d71a31753ab13d883e95aea8f79d5ff3ba3805192029df`.

The [fixture report](evidence/arena-rebuild-bump-2026-11-26/rebuild-fixture/REPORT.md)
separately records all 20 baseline/candidate process results, including the
eight expected baseline failures and initial fixture correction. The
[runtime review](evidence/arena-rebuild-bump-2026-11-26/review/runtime/review.txt)
and [independent audit](evidence/arena-rebuild-bump-2026-11-26/review/ownership/review.txt)
cover interval boundaries, scanner mutation order, every callback initializer
and production transfer reachability. The
[manifest](evidence/arena-rebuild-bump-2026-11-26/manifest.json) preserves hashes
and byte counts for copied evidence and hash-only binaries/archives.

The next stability work remains infallible huge/raw shrink, including active
overflow-stack relimiting, and exact lifetime protection for dead-worker
reclamation. This fix does not change their current completion status.
