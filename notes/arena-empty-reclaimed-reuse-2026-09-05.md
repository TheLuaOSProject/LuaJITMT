# Empty reclaimed arenas: certificate and reuse order

The closure diagnosis found repeated full-arena preparation and quarantine work
on physically retained, empty spares. The final change skips that work using an
authoritative empty-incarnation certificate and a separate atomic empty-spare
head. It retains one adoption attempt per allocation and keeps eligible spares
ahead of unsuitable retained arenas. The real closure workload retains exactly
320 arenas over all five measured rounds; both allocation entry points also keep
exactly four arenas over 32 deterministic collection cycles.

Evidence is in `bench/arena-empty-reclaimed-2026-09-05/`. The `negative-v1-*`
metadata identifies the rejected runtime exactly; the root-level performance and
settled-counter samples in that directory also describe that rejected revision.
They are not release acceptance results.
Final dual-head evidence is under the `reuse-v2/` subdirectory.

## Certificate authority

`LJ_AF_EMPTY_RECLAIMED` is published only by ordinary quarantine completion after
its exact clean generation commit, full terminal-word application, sweep-state
reset, and an independent destructor-kind emptiness check. The last check matters:
a malformed destructor kind outside every block intentionally survives terminal
application. Such an arena can report zero live cells but must not receive the
certificate. The existing terminal-word fixture now checks both cases.

Every certified cell is FREE with no block, root, recovery, physical table token,
or destructor identity. Old mark and cdata coverage scratch need not be zero:
ordinary whole-run adoption repeats its full preflight and clears old coverage
before publishing any new allocation. A fixture seeds both kinds of scratch,
checks they survive eight skips unchanged, then checks same-arena black allocation
and coverage scrubbing.

The shortcut applies only to a traversable arena detached from the CLOSED
reclaimed list. It checks the certificate and lifecycle flags, terminal gate,
pending preparation, deferred reclaim count, remote queue, retire epoch, detached
bump evidence, and the current global descriptor mapping. It then performs one
exact `CLOSED -> SEALED` CAS with zero admissions and no PENDING. It does not clear
dirty evidence to manufacture eligibility. Its successful path changes list
metadata and returns to CLOSED with `keep_pending=1`; no payload, bitmap, or
sweep-epoch update occurs. Existing `sweep_epoch` filtering omits only zero live
cells from cycle accounting.

An actor admitted after that CAS can be counted in the committed generation.
A semantic reader sees FREE/block0 and obtains no readable body. Its count still
blocks later adoption. A late publisher sets PENDING before attempting any bit
publication; unseal must retain both its count and PENDING. The paused shortcut
fixture covers each schedule separately, including reader leave and eventual
same-spare reuse. The late publisher uses the real remote-free API: its stale
body is rejected, while its gate evidence survives and forces ordinary handling
on the next preparation.

The runtime fixture registers a real private arena in a live universe, frees its
incarnation, starts an actual MARK cycle, and protects the old payload page with
`PROT_NONE`. Real recovery and table-token publishers reject the stale pointer
without reading body bytes or changing token/descriptor ownership. This avoids
an IDLE-phase rejection substituting for the intended FREE-incarnation check.

## Invalidation and external readers

| Transition | Certificate treatment |
| --- | --- |
| Mapping requests | The bit is masked out of both ordinary and huge mapping flags. |
| Reclaimed adoption | Clear before seal, drain, staging, or OPEN. Neither early failure nor OPEN rollback restores it. |
| Ordinary PREP, quarantine entry/completion, abort restore | Clear before changing preparation or side-plane ownership. Restore snapshots flags only after that clear. |
| Pending-gate clear or unseal that discards pending | Structural owner clears the certificate before clearing evidence. |
| Terminal reconcile, unmap claim, partial fini | Clear before terminal generation changes; a failed teardown does not restore the certificate. |
| Transfer | Clear before rebinding the retained mapping to a new owner. |
| Allocation, root construction, emitted FNEW, table creation | Existing owner bump/bin paths require prior adoption before a CLOSED spare can gain a new incarnation. |
| Recovery or DESTRUCT | Their lifetime claims reject FREE. Real retained-block plus RECOVERY/DESTRUCT and durable late-free fixtures check the nonempty controls. |
| Exact physical table ownership | Full quarantine authority checks all token lanes; the shortcut also checks the independently mutable global descriptor mapping. |

The new invalidation stores are structural-owner operations. Exported terminal
reconciliation and pending-clear/unseal declarations state that precondition;
they do not become operations for arbitrary concurrent flag writers. The
existing main-TG teardown lifetime rules remain required.

Non-arena flag consumers use explicit masks for existing lifecycle properties.
GC's external PREPSWEEP writes operate on quarantine entries, which already lost
the certificate. The new bit is disjoint from huge magic and cannot enter HugeTab
semantic flags through mapping conversion. No whole-flag equality consumer was
found that would reinterpret the new certificate as a different object kind.

Independent read-only review found no concrete lifetime or admission blocker in
arena C blob `8496230436ad64604312a73a58e2c07179028e77`; header blob
`718f28bccc233acbf2b04f1b6c3f179165877032` adds only explicit structural-owner
comments after the reviewed header. The subsequent retained-capacity measurement
found the separate allocator-ordering blocker below.

## Rejected first revision

The immutable before tree is
`/tmp/lj-gc-jit-combined-20260905-6cxpl6mp/normal`; its binary SHA-256 is
`8b75419d1972794ab6287dc9be8a4aad7b8bdd14f581a0fb4a776e24602bbd75`.
The rejected after tree is
`/tmp/lj-empty-reclaimed-20260905-9vnax2uo/normal`; its binary SHA-256 is
`322876acbb79f546474e592070aa30695b6f99b1e899157be5a70695593f7412`.
Every tracked runtime input matches except `src/lj_arena.c` and `src/lj_arena.h`.
Both trees include coalescing before the later public MARK-scope repair. They
must not be described as the final `d680421c` runtime. The checked-in input
manifest records the full match and parent overlay hashes.

All primary samples keep GC enabled and JIT on. CPU 30 has no SMT sibling;
the governor is `performance`, frequency is not fixed, and the host is shared.
Other functional work used CPUs 0–15, including our focused tests on CPUs 0–11
during part of the measurement window. Each fresh process has a 45-second bound.
The harness reports its best of five internal rounds; those rounds are not
independent processes. Every sample below completed with exit zero.

| Condition | Fresh pairs | Before median ns/closure | Rejected after median | Change |
| --- | ---: | ---: | ---: | ---: |
| Filtered 500k closures | 3 | 1,738.03 | 1,550.95 | 10.76% lower |
| After five 200k-key insertion rounds, then 500k closures | 7 | 3,974.77 | 1,557.04 | 60.83% lower |

The insertion-history multiplier falls from 2.287 to 1.004 in these samples.
However, the separate native diagnostic, linked against each unchanged archive,
shows this settled traversable arena count after successive 500k-closure rounds:

| Round | Before | Rejected after | After certified empty |
| --- | ---: | ---: | ---: |
| 1 | 731 | 329 | 320 |
| 2 | 733 | 459 | 450 |
| 3 | 733 | 591 | 582 |
| 4 | 737 | 721 | 712 |
| 5 | 737 | 853 | 844 |

At every settled row, both builds are IDLE and agree on total bytes (2,857,423),
live estimate (436,160), cumulative allocation bytes, cycle starts, root scans,
root-spine objects, interned strings, and summed swept live cells. The accounting
is stable while physical capacity grows. Timing and logical live-byte counters
alone would therefore have missed this regression.

The source cause is specific. Successful `arena_adopt_reclaimed_one` can open a
retained arena that has no suitable free run. `arena_reserve_bump_impl`, used by
closure-pair reservation, makes one adoption attempt before drain/fresh fallback.
Generic `lj_arena_alloc` has the same bound through its `adopted` boolean. Skipping
empty arenas returns them to the reclaimed list before the nonempty arenas finish
their current sweep; those later completions repeatedly cover the useful spares.

Two deterministic controls reproduce this independently of the benchmark. They
allocate two completely full retained arenas and two real freed spares, run two
ordinary prepare/quarantine cycles, and request storage. No list, flag, or count
is injected. Both generic allocation and typed pair reservation return a fresh
mapping instead of either available spare. Each rejected-revision control aborts
at that exact assertion in under 50 ms; the commands and stderr are preserved.

A proposed tail hint and interior-list insertion were rejected before any such
code was written. GC workers can publish quarantine completions for a TG while
its mutator allocates. A tail observed by the producer can therefore be popped
and relinked into owned storage before an interior next-pointer store. That
would corrupt topology. Increasing an arbitrary head-search limit would also
fail to establish a stable retained-capacity envelope.

## Final concurrent queue design

The final implementation appends one atomic `empty_reclaimed` pointer to
`TGAlloc`. Ordinary reclaimed heads retain their existing meaning; the new head
contains traversable arenas certified empty when enqueued. Both use the existing
concurrent-producer, single-owner-consumer head-CAS discipline. A producer writes
only its private node before publication, never another published node's link.
The successful release head CAS publishes the node. A paused-producer fixture
holds a stale expected head while the owner pops and adopts it, or loses OPEN and
requeues it; the producer then retries its CAS without writing through that old
node.

Adoption checks the empty head first, otherwise the ordinary head, and pops at
most one arena. Its existing seal, full physical preflight, staging, and exact
OPEN checks are unchanged. Every failed adoption clears the certificate and
returns the arena to the ordinary head, so it cannot repeatedly obscure other
available empty spares. Quarantine selects the new head only when the kind is
TRAVERSABLE and the terminal certificate is complete. A direct plain-arena
finish/adopt fixture preserves the public allocator's kind boundary.

PREP atomically detaches both heads. Each detached node is considered once;
clean certified spares return to the empty head, while dirty gates, held readers,
or descriptor conflicts take ordinary preparation and lose the certificate.
Later concurrent publication remains on the corresponding head and is not lost.
The deterministic capacity tests exercise simultaneous ordinary and empty lists
on every cycle.

Registry attachment, terminal reconciliation, both terminal-ready predicates,
partial/complete fini, and dead-source transfer visit both heads. Failed terminal
unmap retains the empty mapping on its head, including when certificate
invalidation has already happened. Dead-source transfer atomically detaches the
additional head, invalidates each certificate, rebinds ownership, and publishes
on the destination's ordinary head. Private initialization and terminal reset
zero the appended pointer, and bootstrap's whole-allocator assignment carries it.
Neither operation may overlap published allocator readers, as before.

Every node on the additional head has `live_cells == 0` until the owner pops it;
terminal certificate invalidation cannot create a live incarnation. GC2's only
external production reclaimed-list walk sums live cells, so it can omit this
zero-contribution head. Capacity and lifetime walkers cannot. Diagnostic fixture
users in arena sweep, runtime sweep, stats, worker scheduling, HugeTab terminal
reconciliation, and terminal TG-orphan tests now include both heads where they
measure physical membership. Live-rescue checks keep their ordinary-head
expectation.

Final source blobs are arena C
`4a55c2e5367cef0e4c5b774e36d2854bbb63672d` and header
`0971b02565feb93af0bfe6be8905f4e052b1a1f5`. The matched normal tree is
`/tmp/lj-empty-reclaimed-20260905-9vnax2uo/reuse-v2-normal`, with CLI SHA-256
`8ab2f53b86e1737bdcc66afcc4718d5ec636f5b80b864d9446c57d7c3dfcb03e`.
Its helper/assert sibling has CLI SHA-256
`f71e8c8375c480843974e81fa50a1ccf9ae2a24acec22c8b16d72d89a10330f2`.
As with v1, the matched runtime baseline is the immutable pre-public-MARK-repair
tree, and only arena C/header inputs differ. These measurements are not a claim
about the later integrated scalar-read candidate.

## Final validation and settled accounting

All seven selected canonical M2 cases pass: bitmap, map, allocation, HugeTab,
sweep, empty-reclaimed, and empty-reclaimed-runtime (47.36 seconds). The related
runtime GC sweep, scalar stats, terminal TG orphan, and parked worker scheduler
fixtures pass. The expanded standalone passes both normally and under target-only
GCC TSan on CPUs 0–3. It includes exact post-shortcut reader and late-publication
schedules, failed adoption before seal and after staging, reciprocal recovery and
DESTRUCT owners, stale descriptor rejection, registry attachment, terminal and
transfer handling, the plain-kind control, and the producer/consumer CAS schedule.
The same known fence-instrumentation warning is the only warning demoted.

The final settled diagnostic retains 320 traversable arenas after each of five
500k-closure rounds: 311 certified-empty and nine ordinary retained arenas.
The baseline retains 731, 733, 733, 737, and 737 respectively. Every settled row
is IDLE; both builds have equal cycle starts and root scans. Final settled total
bytes are constant at 2,857,487 and live estimate at 436,192. Baseline values are
2,857,443 and 436,160. These cross-build offsets are reported explicitly: the
layout probe shows TGAlloc growing 688→696 bytes and TGState/GG_State each growing
16 bytes, and the diagnostic's package-path strings also have different lengths.
They are not repeated-cycle growth. The runtime fixture separately checks exact
before/after byte and counter equality across 64 skips without any allocation.

One expanded-fixture attempt reached the registry case and failed because its
private HugeTab wrapper lacked the required zero initialization. The fixture now
initializes it explicitly; the failed output is retained with the passing final
commands. No final validation or settled workload timed out.

The final normal performance pairs use the same GC-enabled harness, scales,
process bounds, and CPU as the rejected revision, with fresh processes and
alternating before/after order. Own functional work was complete before these
primary samples; other agents could still use CPUs 0–15 on the shared host.

| Condition | Fresh pairs | Before median ns/closure | Final median | Change |
| --- | ---: | ---: | ---: | ---: |
| Filtered 500k closures | 3 | 1,740.75 | 1,523.25 | 12.49% lower |
| After five 200k-key insertion rounds, then 500k closures | 7 | 3,976.11 | 1,530.41 | 61.51% lower; 2.598× speedup |

The final insertion-history multiplier is 1.005, versus 2.284 before. All 20
primary processes completed with exit zero; none timed out. The post-insertion
before range is 3,960.22–3,997.53 ns/closure and final range 1,529.38–1,534.71.
This demonstrates the targeted allocator cost reduction on these matched builds.
It does not establish full-harness parity with stock or measure the later
integrated scalar-read repair. GC remains enabled throughout the primary and
settled measurements.

## Validation of the certificate revision

The rejected revision passed the registered M2 bitmap, map, allocation, sweep,
empty-reclaimed, and empty-reclaimed-runtime cases (42.26 seconds in an isolated
suite tree). The focused stats and runtime GC sweep fixtures also passed. Its
expanded standalone fixture, including both post-CAS schedules and late RECOVERY
and DESTRUCT controls, passed GCC ThreadSanitizer with no runtime diagnostics.
Only GCC's known `atomic_thread_fence` instrumentation warning was demoted with
`-Wno-error=tsan`; no host settings were changed. This supports the authority
checks but does not override the independently demonstrated capacity failure.

An initial standalone link attempt incorrectly added `LUA_USE_ASSERT` to the
TLS-stub-only build and failed for the missing runtime assertion helper. Its
exact failure is retained. The canonical standalone flags keep ordinary C
assertions enabled and omit that runtime-only macro. There were no timeouts in
the completed certificate validation or primary measurement samples.
