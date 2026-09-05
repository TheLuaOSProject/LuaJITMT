# Acceptance checkpoint — candidate rejected for mixed-owner starvation

Do not integrate `candidate.patch` (SHA256 `6904c48d7d24ad9776c390c3618d7cb0b562ba96bafab124be0d58bc8b22cb44`). The exact three-file candidate is unchanged from the first focused checkpoint. It fixes the original full-collector self-wait, but its global defer event ends the TG traversal before independent eligible owners receive a turn. A persistent root owner can therefore delay their work indefinitely. No shared source or combined automatic-control variant was used.

## Positive retained-work evidence

`acceptance/v2/t-owner-defer.c` follows the existing constructing-upvalue helper at `t-gc-root-pending-race.c:242-261`. It uses the production allocator, real initialized upvalue bytes, and the production publish/cancel APIs. It never writes root/lifetime/READY/phase/gate metadata. A child table remains rooted during construction; after publication its only root is the anchored upvalue, and its sentinel value must survive collection.

Candidate publish, cancel and automatic runs all pass:

- Nested full collect returns 0 in under 0.5ms with exactly one new deferred event. The exact allocation remains block=1, root LINKING=1, lifetime CONSTRUCT=2, READY=0, in the same quarantined arena. PREPSWEEP is clear after return, and the existing cursor remains 2214.
- Successful production publication gives MEMBER=3 / LIVE=1 / READY=1. Full collection then returns 1, reaches IDLE, removes quarantine, empties the complete SSB predicate, and preserves the child-table sentinel through the upvalue edge.
- Production cancel/free gives root NONE=0, lifetime LIVE=1, late=1 and retirement epoch UINT64_MAX. This is the allocator's real irrevocable late-free handoff with a fresh grace requirement (`lj_arena_quarantine_owns_body:5882-5922`), not immediate physical FREE and not necessarily a remote-free queue entry. Full collection subsequently returns 1/IDLE with quarantine empty.
- Automatic mode enters SWEEP through bounded `lj_gc2_step_explicit(L,1)` calls and stops before the first physical owner quantum. One `lj_gc_step` returns -1 with one event, retaining the constructor. Publication then permits completed collection. This isolates the outer automatic event-stop behavior without requiring subsequent calls to keep an arena quarantined when its unchanged finish protocol may lawfully preserve an owner.

The unchanged baseline fails the new publish and automatic deferral assertions: it can finish the arena while retaining the raw constructor. This is direct evidence of the per-call pending/arena geometry distinction, not an explanation retroactively assigned to historical passes. The baseline's original untouched function fixture still times out at 60s in the first checkpoint.

## Preserved diagnostic generations and oracle corrections

Nothing was overwritten to erase a failure:

- `acceptance/` first constructing fixture: publish passed. Cancel incorrectly asserted immediate FREE; observed LIVE with root cleared. Automatic incorrectly required every subsequent call to defer; the observed next call legitimately completed the arena while preserving the owner. Both assertions failed and are preserved.
- `acceptance/v2/` adds the exact late/fresh-grace cancellation oracle and places automatic entry before its first owner encounter. Three candidate positive cases pass; two unchanged-baseline negative cases fail their explicit deferral assertions.
- First mixed fixture, `acceptance/t-mixed-owner.c`: worker-zero passed useful progress. Worker-two also advanced the eligible arena, but the new fixture incorrectly required another deferred event after the constructor's arena had already lawfully finished; that assertion failed. It is not evidence of worker starvation or an idle worker bug.
- `acceptance/mixed-v2/` establishes a real persistent EOF owner before judging progress. Both worker-zero and worker-two fail only the final independent-owner progress assertion, after cancellation, completed collection, sentinel validation and cleanup all succeed. These are the substantive candidate acceptance failures.

All tests retain a 60-second process bound. New focused call/window assertions use an explicit one-second upper bound; actual foreground calls are sub-millisecond and the worker observation window is roughly 158ms. Exact scripts, compile argv, helper macros, archive/source/ELF hashes, outputs, exit codes and timing are preserved for every generation. An intentional negative assertion is distinguished from a rejected candidate acceptance assertion in the descriptions above.

## Concrete persistent-owner / independent-work failure

The mixed fixture creates a real attached publisher TG and an unrelated rooted main-TG table in another arena. Main's arena also contains discarded tables eligible for collection. To retain one constructor at the tail of an exhausted publisher arena, the publisher creates ordinary upvalues, publishes each preceding one through `lj_gc_linkobj_new` and `lj_gc_pubobjroot`, and holds only the last. At most two private constructors coexist. Allocation naturally crosses to another arena after 1159 prior publications; the extra allocation is cancelled. No allocator cursor or GC plane is modified.

The held upvalue is at cell 4093 of 4096, size 48. Its nested collection returns deferred. Its arena's actual reclaim cursor has reset to FIRST_CELL=616, which is asserted as the persistent EOF dependency precondition. The main arena is already quarantined at epoch 0 while the active cycle is 1; its real rooted table keeps the mapping valid for observation.

| Driver | Held window | New events / parks / owner runs | Independent arena |
|---|---:|---:|---|
| 64 separate bounded worker drains, workers=0 | 29135ns | 64 / 0 / 64 | epoch 0; not completed |
| Two real background workers | 157624401ns | 273 / 274 / 273 | epoch 0; not completed |

In both runs, arena completion count remains zero, the exact held allocation remains LINKING/CONSTRUCT/READY-clear and block-present, and the cursor remains 616. The worker backoff is bounded and actually parks; that prevents CPU spinning but does not give the later eligible TG a turn.

After the real owner cancels the allocation, full collection returns 1/IDLE, publisher quarantine empties, the main arena advances (the explicit full request also performs a major cycle, ending at epoch 2), and the independently rooted sentinel survives. Only then does the final `assert(eligible_done)` report that independent work incorrectly depended on blocker release. Both failures preserve actual cleanup/completion evidence rather than aborting before release.

## Source implication

`lj_gc2_sweep_owner_progress` stops after the exact root-owner encounter and emits the global event after writer cleanup. The enclosing TG loop then ends on that event. Every next invocation restarts at the same TG and same retained EOF retry. The work remains safe and helpable after cancel/publication, but the traversal monopolizes all other eligible TGs while this owner stays suspended.

The next candidate should retain a local per-owner blocked result until a bounded fair pass has offered independent eligible owners a turn, then publish one event before returning to top-level drivers. It must keep the same construction gates, per-arena retry identity, physical cleanup, work counters and finalization authority. A local blocked result is not permission to clear quarantine or fabricate completion. Further source design is isolated in the next generation; this rejected candidate remains frozen.
