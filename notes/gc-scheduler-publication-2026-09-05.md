# Worker scheduler fixture publication, 2026-09-05

The scheduler fixture could fail while workers had already drained all the
work published to them. Its async MARK subtest ignored a zero return from
`lj_gc2_flush_ssb`, then waited for global SSB emptiness. That flush is a
bounded attempt: when the owner has no spare buffer and recycling cannot
obtain one, it leaves the current suffix private. Sleeping in the fixture
does not publish that suffix, and background MARK workers only rotate their
own buffers.

Two diagnostic generations preserve every original predicate, assertion and
timeout. The first prints state only after the old wait expires; the second
also retains the return value of the already-existing flush call. Across
120 interleaved fresh processes, seven reproduce the same assertion on the
pristine `597b8705` control, initial automatic-admission candidate and STOP
veto. All seven show exactly one main-owner GCRef still private, empty
published/grey/recovery work, empty worker suffixes, and no handshake or
MARK-close intent. The five failures with the flush-return observation all
record zero. No other assertion fails and no diagnostic process times out.

These individually acquired observations and the [source comparison](evidence/gc-scheduler-publication-2026-09-05/diagnosis/review.md)
establish a missing fixture publication precondition on all three variants.
The original isolated failure did not capture its internal state; this does
not retroactively observe that run's exact refusal branch. The earlier
unresolved classification and raw failure remain preserved in the
[automatic-control review](gc-auto-control-review-2026-09-05.md).

The permanent fixture now requires owner publication before observing worker
drain. A small local helper accepts an already-empty suffix or a successful
existing flush; on refusal it retries at most 1,000 times with a 1 ms sleep.
The parent/child/grandchild graph, mark calls, every worker/semantic assertion,
both original observational waits and their bounds are unchanged. This is a
separate bounded publication step. It does not clear a cursor, manufacture
queue state, take an extra root token, or invoke a full GC drain.

All 60 corrected full-fixture runs pass against the three matching frozen
helper archives. Six deliberate negative controls reject the expected error:
forced flush refusal exhausts exactly 1,000 calls and fails publication;
falsely claiming publication leaves the private entry and fails the unchanged
global-empty assertion. Exact flags, source/archive/binary identities and
all outputs are retained in the [correction handoff](evidence/gc-scheduler-publication-2026-09-05/correction/HANDOFF.md).
There is no new ASan or performance claim for this fixture-only change.

The integrated canonical `m3_gc2_worker_scheduler` entry passes its C fixture
and both JIT modes of `t-gc-workers.lua`, including default build preparation,
in 48.840 seconds. [Final validation](evidence/gc-scheduler-publication-2026-09-05/root/final-validation.json)
binds those three additional processes to the permanent fixture SHA-256
`f476733106d7e2552c9e47757b35f1c1dc78cf8b3cb0d47c17f17ed58fe70dca`.
The archive preserves 519 text artifacts and 25 hash-only identities.

No production runtime source changes. The independent two-worker SWEEP
completion bound, public STOP/restart publication, closure-construction wait
and concurrent string reclamation remain separate work. Validation is Linux
x64 only.
