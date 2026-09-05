# Constructor deferral and independent-owner starvation

The first reclaimer deferral candidate is rejected. It makes a nested full
collection return while preserving an unfinished constructor, but its global
defer event prevents an unrelated eligible TG from receiving sweep work.
Bounded retries and worker parking do not repair that dependency. No source
from this candidate was integrated.

## Original collector self-wait

The unchanged `t-func-construction-anchor.c` fixture injects full collection
after a real open upvalue has been published while its enclosing raw closure
remains unlinked. The constructor cannot publish or cancel that closure until
the nested call returns. The exact fixture hash is
`42e9d47359fb27565565fe07678abbc8de7abf119226075cab3a1142768a6f0f`.

In captured runs, the collector reaches SWEEP with empty logical work queues.
The exact closure remains block-present, marked, LINKING/CONSTRUCT and not
READY in the same quarantined arena. Reclamation correctly avoids its mutable
header, but cursor advancement counts as work and repeatedly drives the same
arena. Roughly three seconds produce 843,243 or 844,833 owner runs, no arena
completion and no deferred event. The unchanged 60-second fixture bound is
reached in matching control/candidate configurations.

This is a production collector contract problem exercised by a test-only
nested-collection hook. Ordinary `func_finduv_nothrow` does not call full
collection there; an ordinary Lua trigger for this exact schedule is not
claimed. An older four-helper executable passed, but the fixture discards the
nested return and accepts either a changed cycle or active phase. That pass
does not prove the nested cycle completed. Later matching four-helper control
and GC-control-candidate runs both time out. The helper-macro difference is
not established as the cause.

The scanner's `pending` flag is local to each bounded call. Encountering the
owner in the EOF call forces cursor reset. Encountering it earlier can allow
a later EOF call to preserve the owned bitmap span and complete the arena.
That behavior explains why a persistent-tail precondition matters in new
tests; it is not a retrospective explanation of an unobserved historical
return path. Optimized debugger line mapping also prevented one intended
LINKING-branch breakpoint from observing that exact branch. Those captures
prove constructor/quarantine identity, not branch execution.

## Rejected candidate and retained-work checks

The isolated three-file patch is
`6904c48d7d24ad9776c390c3618d7cb0b562ba96bafab124be0d58bc8b22cb44`,
based on the frozen pristine `597b` control. It changes `lj_gc.c`, `lj_gc.h`
and `lj_gc2.c`, without the later automatic-control or FFI changes.

The scanner reports an exact LINKING/UNLINKING observation through an optional
output, preserving its old wrapper and all bitmap, cursor, EOF, ownership and
finish behavior. After physical writer cleanup, the owner call emits one
existing deferred event and stops. The TG traversal and outer automatic batch
then honor that event. The original fixture passes in 0.0155 seconds, while
the newly linked unchanged control times out at 60 seconds.

Focused real allocations prove more than a passing fixture:

- Nested full collection returns zero promptly with a new deferred event. The
  same allocation remains LINKING/CONSTRUCT/READY-clear and quarantined.
- Production publication makes it MEMBER/LIVE/READY. Subsequent collection
  reaches IDLE, clears quarantine and preserves a child-table sentinel held
  only through the published upvalue.
- Production cancellation clears the root claim and hands off late freeing
  with a fresh retirement epoch. LIVE can remain until physical completion;
  subsequent collection reaches IDLE and clears quarantine.
- An automatic call positioned before its first physical owner encounter
  returns on the deferred event. Publication then permits full completion.

Initial cancellation and repeated-automatic assertions were wrong and remain
archived. Cancellation need not free immediately, and a later scan can
lawfully finish an arena while retaining the owned allocation. The corrected
checks observe the real release/retirement protocol and preserve the required
eventual completion. No READY, root, lifetime, phase or gate word is fabricated.

## Required independent-work check fails

The final mixed fixture creates an attached publisher TG and an unrelated
main-TG arena containing eligible garbage and a rooted sentinel. Ordinary
upvalue allocations/publications fill the publisher arena until one real
48-byte constructor occupies cell 4093 of 4096. At most two private
constructors coexist; no allocator cursor is written. The first nested
collection returns deferred with that arena's cursor reset to FIRST_CELL 616,
establishing a persistent EOF dependency.

| Driver while constructor stays held | Observation | Independent arena |
| --- | --- | --- |
| 64 separate bounded drains, no background workers | 64 events, 64 owner runs, zero arena completions | Epoch remains zero |
| Two real workers for about 158 ms | 273 events, 274 parks, zero arena completions | Epoch remains zero |

Both cases preserve the exact held allocation. After its owner cancels,
collection completes, the other arena advances and its rooted sentinel
survives. The final `eligible_done` assertion then fails because independent
work did not progress during the held window. These failures include cleanup
and later completion evidence; they do not abort before releasing the owner.

Every new invocation restarts from the same TG. Its global defer event ends
the list before later eligible owners are considered. This is a demonstrated
candidate starvation defect, distinct from the earlier mixed-fixture error
that expected another event after an arena had already lawfully completed.

The next design must aggregate root-owner refusal locally, give other owners
bounded turns, and preserve a continuation across work-budget boundaries.
Simply delaying the event until the loop ends still permits head starvation
when the first owner consumes the remaining quota. Any scheduling hint must
resolve through current TG membership and retain existing lifetime/writer
gates; it cannot grant reclamation authority. Same-TG arena fairness remains
a separate requirement. The revised design is isolated and unvalidated by
this review.

## Frozen evidence

[evidence/gc-construction-defer-review-2026-09-05](evidence/gc-construction-defer-review-2026-09-05/)
contains the exact diagnosis, debug observations, original fixture and source
copies, first focused comparison, all acceptance generations and rejection.
Its manifest rechecks these immutable owner manifests:

- Diagnosis: `520715455c2b82e9ad800621bf03be1a1300957ccf5b521d9a2419f94895532e`.
- Focused candidate: `1c95ae02cd52c065634b7fc6a68c34ed8544c2106a5c34d7e7215ab4b4fde1ee`.
- Rejected acceptance: `30aed87c54ccf6274ecda0b5948363c5c83ae59991a59db9e5924f1965ec53d7`.

Only verified UTF-8 text is copied; executables, objects and archives are
hash-only. Debugger interruption is never counted as a fixture pass. This
review changes documentation only and does not establish asynchronous GC,
worker SWEEP completion, or release readiness.
