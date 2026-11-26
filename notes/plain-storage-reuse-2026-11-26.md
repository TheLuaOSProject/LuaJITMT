# Reclaim explicitly freed plain storage

This review starts at `0c6d00bd1097e31e06bf0b7cbbf8cdbd2b408223` and prioritizes
Linux stability, bounded physical memory and performance. A sustained buffer
refill test exposed retired storage that remained allocated until `lua_close`.
The final change returns that storage to the owner's allocator after its readers
leave, without treating unmarked raw allocations as garbage.

The existing buffer replacement correctly frees its old backing while retaining
a storage lease. Releasing the lease first would allow the allocator routing
read to race physical reuse. The lease prevents immediate free, so the allocator
publishes the backing's `late` bit. However, the production collector prepares
and sweeps traversable arenas only, and the old remote-free drain consumed only
intrusive queue records. Plain late bits had no production consumer.

The baseline census found 3,870 plain arenas and 50,250 retired allocation starts
after 200,000 refills. Five full collections left those allocations intact. At
800,000 refills RSS reached about 969 MiB while Lua accounted for less than
1 MiB. GC cycles continued and returned to IDLE. The remaining storage was
released at state destruction, proving universe-lived retention rather than an
accounting display issue.

The owner drain now processes explicit plain late bits after intrusive queues.
It tries the existing plain writer admission, skips retained arenas without
waiting, validates the entire allocation span and inserts the freed run through
the existing bin protocol. It derives the extent from the next bitmap boundary
and the active bump cursor. An earlier reused interior bump cannot truncate a
later allocation, and an abandoned tail retains its boundary. Non-late blocks
remain allocated regardless of their mark bits. Foreign releases never edit
owner bins.

Each routed plain late publisher release-publishes the existing allocator wake
after its durable bit. A drain that consumes the wake sees that bit; if an
admitted publisher or reader prevents writer admission, the drain rearms the
wake. A publication after the exchange leaves a new wake. This covers local and
remote frees, deferred frees, zero-size realloc, allocator-callback routing,
side-owner refusals and sweep restoration. Parameterless internal destructor
and quarantine APIs retain the forced owner-GC/transfer fallback; production
raw frees use the routed APIs. GC owner checkpoints also drain explicit plain
retirements. No arena layout, bin classification or buffer lifetime rule changes.

An initial version forced a complete scan whenever allocation exhausted its
bump and bins. That bounded buffer memory, but a separate live-heap test caught
quadratic scan cost: 4,096 live arenas cost about 4.7 times more to populate.
The final wake protocol restores empty-drain elision. The regression checks
that growing a heap without retired storage probes zero existing arenas. The
five paired final timing samples have a median ratio near 1.01 at 4,096 arenas;
all rows, including timing variation, remain in the evidence.

The final paired buffer measurements use three fresh processes per variant and
policy, on CPU 8. Each process performs 40 rounds of 20,000 reset/put/get cycles
with a 1 KiB string and checks every returned payload. Values below are medians
of process means; explicit GC time is recorded separately.

| GC policy | Previous µs/cycle | Final µs/cycle | Previous / final RSS after 800k |
| --- | ---: | ---: | ---: |
| Automatic | 6.076 | 0.572 | 969 / 5.0 MiB |
| Full collection every 20k | 6.340 | 0.603 | 969 / 5.2 MiB |
| Full collection every 100k | 6.527 | 0.584 | 969 / 4.9 MiB |

The final census holds at five plain arenas after 200,000 cycles, with 19
pending late starts. Explicit buffer free leaves one retired backing; the first
full collection clears it, and all five subsequent full-collection snapshots
contain zero late starts. The baseline still retains 50,251. Small arenas remain
mapped as reusable capacity; bounded reuse does not require unmapping every
arena. The compared processes each have one OS thread. These measurements do
not establish a multiworker heap bound or stock-performance parity.

The [final measurement report](evidence/plain-storage-reuse-2026-11-26/performance/final/REPORT.txt)
includes every policy row, synchronized `/proc` snapshot, own-TG arena/bin
census, after-close observation and exact input/binary hashes. The initial
leak diagnosis and rejected forced-scan variant are preserved separately.
[Live-heap samples](evidence/plain-storage-reuse-2026-11-26/review/live-growth-final.json)
retain all 40 timing rows; the earlier forced-scan comparison retains 24.

The new `m2_arena_plain_late` gate exercises real local and foreign storage
leases at multiple sizes, hinted-drain rearming, all five free/realloc entry
paths with readers and side owners, a whole-span READY veto, an interior bump,
and live unmarked storage across full collections. Deterministic pauses cover
writer-commit loss, a publisher stopped before its bit store, and publication
after the owner consumes the wake. Allocation elsewhere completes before the
stalled publisher resumes. Public buffer refills stay bounded with automatic
GC disabled, in interpreter and JIT modes. The original reader-release
regression aborts against the previous commit and passes after the fix.

GCC release, Clang assertions, ASan, TSan, and builds without FFI, JIT or buffers
pass the applicable checks. Release/assertion/ASan each pass 509 stock JIT source
cases, 387 interpreter cases, 509 independently produced stock-bytecode cases
and the shared-runtime CI workloads. Assertions and ASan also pass full GC
traversal; the allocator race fixtures pass under assertions, ASan and TSan.
The canonical runner checks the new gate and the allocator/GC integration,
then restores the default shared build. ASan leak detection is disabled;
TSan does not instrument generated JIT machine code.

The [verification record](evidence/plain-storage-reuse-2026-11-26/review/final-verification.json)
checks all 228 runtime inputs per build, executable/library identities and 83
direct runtime invocations against the final workspace. Composite CI invocations
contain further tests. An initial standalone assertion-test driver omitted
`lj_assert.c`; the retained linker failures disappear after adding that source,
and all final standalone checks pass. The
[manifest](evidence/plain-storage-reuse-2026-11-26/manifest.json) distinguishes
copied text evidence from hashes of large binaries and source archives.

The project is not ready for a release. Plain body mutation still uses an
arena-wide exclusive admission state: a suspended writer can deny a new reader
of another live allocation in that arena. This change uses that existing
lifetime protocol and does not prove full runtime lock freedom. The next
progress review must address that scope while preserving exact storage reuse,
then extend physical-memory measurements to active workers, ownership transfer
and repeated detach. Large-bin search and publication overhead also remain
performance work. Windows and macOS validation stay deferred until a release
candidate is ready.
