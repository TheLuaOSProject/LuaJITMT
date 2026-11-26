# Preserve long free-bin chains

This change starts at `c106cd7bb109e9705c6b753f794001627320ab7f`, after the
small-shrink stability fix. The retained-capacity controls exposed a separate
allocator defect: valid free runs disappeared from the bins once a chain grew
past 8,192 entries. The bitmap still described free memory, but ordinary PLAIN
allocation did not rediscover it. An explicit owner rebuild recovered the
candidate's missing capacity; that was diagnostic evidence, not a production
reclamation mechanism.

Insertion used to walk every existing node to remove possible duplicate
addresses, then cut the suffix at its fixed walk limit. Search used the same
limit to discard the entire bin. Thus the limit treated a large valid heap as
corruption. It also made bulk freeing quadratic before the limit was reached.

The current ownership protocol makes that complete insertion scan unnecessary.
Owner, remote-queue and late-drain frees require an allocated start and the
existing terminal authority before publishing a free run. A managed realloc
suffix belongs to its claimed live allocation. Allocation removes the complete
selected node before consuming its cells; the remainder becomes a private bump
window. Rebuild clears bin heads or stages an arena absent from those heads.
These paths do not lawfully insert a span already in a bin. The older split-tail
and free implementations that motivated the duplicate scan lacked parts of
this protocol. The existing deliberately injected stale-head regression remains
supported and unchanged.

Insertion now stops at the first valid head. It still removes stale leading
nodes, with that cleanup amortized over the removed nodes. Ordinary valid
insertion is constant-time. Search completes the finite candidate list before
mapping another arena or reporting allocation failure, while preserving all
existing bitmap, ownership, lifecycle and reader checks. A fitting run beyond
the old limit remains reachable in the same allocation call.

A streaming cycle detector replaces the length cutoff on both walks. It
compares pointer values already encountered by the ordinary traversal and
never follows an unvalidated second pointer. Actual cycles terminate; long
acyclic lists remain intact. Header and thread-state layouts, bin classes,
admission gates and full-span free preflight are unchanged.

This removes quadratic valid insertion and capacity loss from the artificial
limit. A failed search is still linear in the number of examined nodes, and
repeated requests behind unsuitable or gated runs can still repeat that work.
Finite owner-private traversal does not wait for a suspended peer, but this
change is not a constant-work allocation claim. A FIFO rotation with a fixed
budget was deferred: preserving links alone can still map fresh arenas forever
if the search repeatedly stops before a fitting run. Any later search index or
continuation scheme must prove both capacity discovery and progress.

Review also found an independent rebuild defect. With an interior private bump,
the scanner excludes everything after its cursor instead of only the private
window. A later free run can therefore disappear during an explicit rebuild.
That issue is separate from the insertion/search cutoff and is not repaired by
this patch. It remains an obligation, together with repeated linear misses,
retained live-shrink capacity and the worker-exit reclamation gaps.

The ownership review also distinguishes duplicate late intent from duplicate
intrusive queue publication. Ordinary PLAIN remote free still transfers one
raw allocation ownership exactly once. Calling that raw free twice can corrupt
its queue; the old bin scrub did not repair that condition. Production buffer
replacement frees removed backing only after winning its range CAS. Supported
duplicate late bits and repeated terminal frees before reuse are covered by
the regression, without assuming that the PLAIN queue itself deduplicates.

## Capacity and lifetime regressions

`tests/t-arena-bin-capacity.c` allocates 9,000 real 1 KiB bodies separated by live
16-byte guards, consumes the original bump tail with real allocations, frees
the targets and checks the complete bin inventory. Refill must return every
original address exactly once without mapping another arena. Five cases cover
local and foreign PLAIN/TRAVERSABLE frees and PLAIN late intents behind genuine
admissions. Search cases put a real 2 KiB fit after 8,192 undersized nodes in
the same bin, then verify that an unsuccessful search preserves the nonfits.
Only the separately named defensive-cycle case fabricates corrupt links.

| Check | Baseline `c106cd7bb` | Candidate |
| --- | --- | --- |
| Five capacity cases | 8,193/9,000 reused; 524,352/581,915 free cells; arenas 170 to 185 | All 9,000 reused; all free cells accounted for; 170 arenas |
| Two fitting-tail cases | Misses position 8,193, drops the 8,192 nonfits, maps another arena | Returns the exact fit and retains all nonfits |
| Deliberate stale-prefix and nonfit cycles | Terminates | Terminates |

The seven lawful baseline failures are ordinary exit 1 with diagnostics. The
[fixture report](evidence/plain-bin-capacity-2026-11-26/bin-fixture/REPORT.md)
preserves source identities, commands and individual results.

One initial ASan validation run failed an existing remote-publisher test's
`drain == 8` assertion. It was not a sanitizer finding. A forced schedule
reproduces that assertion on both the baseline and candidate: one publisher
pauses after its queue CAS while retaining admission; the other seven publish
durable late intents and preserve their LIVE bodies. Immediate drain correctly
consumes only the queued record. Real sweep preparation and quarantine then
reclaim all eight, and subsequent allocation reuses their exact addresses.
Black allocation and a marked live guard ensure that explicit free intent,
rather than unmarked collection, accounts for reclamation.

The corrected `t-arena-remote-pending.c` forces that schedule and checks every
payload, state transition, exact address and unchanged arena count. It passes
against both frozen ASan runtimes. The original failure, deterministic negative
controls and source correction are retained in the
[deferral review](evidence/plain-bin-capacity-2026-11-26/remote-pending/review.txt).
The seven runtime build trees remain immutable; the changed fixture is frozen
separately and replaces only its own three final validation runs.

## Validation

All seven Linux configurations build: GCC release, Clang assertions, Clang
ASan, GCC TSan, no FFI, no JIT and no buffer. There are 88 passing current
runtime invocations: 8 GCC, 27 assertions, 27 ASan, 21 TSan, 2 no-FFI,
2 no-JIT and 1 no-buffer. These counts treat each stock/CI matrix process as
one invocation. Each of GCC, assertions and ASan passes 509 stock source cases,
387 eligible interpreter cases and 509 independently produced stock-bytecode
cases, plus the shared Linux CI workload. Public buffer, paused-reader,
late-free, shrink, lease and GC traversal coverage remains in the matrix.

Ten canonical arena gates pass, including the new capacity gate. The affected
publication gate additionally passes with the corrected fixture. ASan leak
detection is disabled; TSan does not instrument generated JIT machine code.
Windows and macOS remain deferred until release preparation.

Each build records 1,236 source inputs, including 228 runtime inputs. Only
`src/lj_arena.c` changes among the runtime inputs. Final verification checks
all frozen files, each emitted binary and the separately frozen corrected
test. The canonical build restores the normal workspace executable/library
configuration. [Final verification](evidence/plain-bin-capacity-2026-11-26/review/final-verification.json)
retains the two original-fixture passes and its initial ASan assertion failure
as superseded results rather than deleting them.

## Performance and retained capacity

The [paired report](evidence/plain-bin-capacity-2026-11-26/performance/REPORT.md)
contains 104 passing performance invocations against `c106cd7bb`. All raw rows,
commands, input hashes and timing variation remain available. These focused
measurements do not establish stock parity or whole-runtime lock freedom.

| Workload | Baseline | Candidate |
| --- | ---: | ---: |
| Free 9,000 guarded 1 KiB PLAIN bodies, median ns/free | 9,362.5 | 313.8 |
| Free 9,000 guarded 1 KiB TRAVERSABLE bodies, median ns/free | 23,709.4 | 229.5 |
| 4 KiB allocate/touch/free, reserved-core repeat, median ns/cycle | 722.3 | 691.3 |
| Public 1 KiB buffer reset/put/get, median ns/cycle | 595.6 | 564.3 |

The reserved-core 4 KiB paired ratio is 0.9573, with all five pairs in
0.9537–0.9624. The original shorter samples varied much more. An intervening
longer set may overlap a diagnostic on SMT sibling CPU 9; it is preserved and
labeled accordingly. The final repeat reserves CPU 8/9, with validation on
CPUs 0–7. Frequency, shared-cache and host interference remain uncontrolled.
The ordinary 64-byte and 256-byte controls also improve; full ranges are in
the report rather than summarized as general application speedups.

The maximum retained-shrink controls still keep 32 MiB of physical storage
for 128 KiB or 32 KiB logically live. This patch does not reduce that cost.
After all original and refill allocations are freed, however, candidate bin
plus bump bytes account for every usable cell without rebuild or transfer:

| Original block size | Owned arenas | Published free bytes | Private bump bytes | Complete usable bytes |
| --- | ---: | ---: | ---: | ---: |
| 4 KiB | 1,062 | 59,127,680 | 4,480 | 59,132,160 |
| 16 KiB | 1,176 | 65,462,912 | 16,768 | 65,479,680 |

The baseline exposes only about 8.8 MB in its bins. Candidate bulk-free CPU
time in these controls falls from 2.432 to 0.011 seconds and from 1.335 to
0.012 seconds. Both versions retain their owned mappings, around 69/76 MiB
RSS after refill and free. Reusable inventory and physical unmapping are
separate obligations, as is the documented dead-worker/FINREG retention.

## Evidence and next work

The [manifest](evidence/plain-bin-capacity-2026-11-26/manifest.json) records
byte counts and SHA256 for copied evidence and hash-only binary/source
archives. It includes build logs, raw validation and benchmark output, the
original test failure, review snapshots and scripts. Runtime source SHA256 is
`1a3420fe54b2450bedfeb0bca775f2625e6452459ac1da94598eac5a4acbfe29`.
Frozen GCC library SHA256 is
`2a29b71710786af7ee2f8a06eb402e1923ea72e31fba6d4d7ee390d05a7b0b28`.

Next stability work is to subtract only the exact private bump interval during
rebuild, make huge shrink infallible without invalidating readers, and reclaim
dead-worker metadata under exact lifetime protection. The revised FINREG
proposal is saved with the [worker census](worker-retention-2026-11-26.md);
it is a design, not an implemented reclamation claim. Search indexing and
live-shrink capacity reduction still need performance work with the current
capacity and suspended-actor checks preserved.
