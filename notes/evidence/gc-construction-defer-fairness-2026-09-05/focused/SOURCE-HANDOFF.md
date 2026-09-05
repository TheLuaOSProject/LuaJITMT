# Fair TG sweep candidate — frozen source checkpoint, tests pending

Full four-file patch: `candidate.patch`, SHA256 `a62a8251ba8315f8ce07871635f1367659be3136895102776955c7204a4e018a`.
Delta from rejected candidate: `fair-delta.patch`, SHA256 `16d1bbc6eedd4aa17f0949b60307a94281334b671db3f77c34d7a51c51a16089`.

The source remains based on pristine 597b plus the isolated owner-deferral work. The automatic-control candidate3/shared tree is not an input. No shared source or build changes were made. This checkpoint freezes implementation before original/retained/mixed/lifecycle validation; it is not release-ready evidence.

The per-owner exact LINKING/UNLINKING refusal is now a local optional result after the unchanged writer/unseal cleanup. The standalone test wrapper preserves its existing immediate event behavior. The real worker TG pass aggregates local refusals, offers independent owners bounded turns, then emits at most one root-defer event if another consumer has not already changed the sampled epoch. Existing full/explicit/automatic/worker event handling remains.

One uint32 scheduling hint stores the next TG ID; no TG pointer survives an invocation. It is placed after grey_capacity, in the expected x64 alignment gap before grey_top; actual layout must be checked before claiming unchanged offsets. The worker token serializes its accesses. Lookup resolves the hint through the captured current list, and an absent ID falls back to its head. Traversal visits that captured list at most once in circular order. Before any work-quota, finished-arena or deferred boundary, it records the next current TG ID, so the next invocation does not restart at the blocked owner.

`limit` is the EXISTING WORK QUOTA, not a new exact cell or time bound. Each owner iteration can include a reclaimer call with up to 64 scan units (ordinary cells or certified no-op bitmap words), remote draining and other guarded work. Returned work and all original counter branches remain. The cumulative quota and the existing end after one finished arena remain unchanged. This candidate makes no new raw-cell bound claim; the earlier source proposal's shorthand 'raw-work budget' must be read as this existing quota.

## Captured-list proof

A search across all source `.c/.h` inputs finds these runtime legacy-list writes:

- `lj_gc2_init` initializes the list before runtime use.
- `lj_tg_attach` CAS-prepends a newly attached node; its next link is established before head publication. Capturing the old head excludes later prepends until the next invocation.
- Its existing-node path can repair `next == self` to NULL. Both lookup and traversal explicitly treat a self-next as a tail. Observing either pre-repair self or post-repair NULL therefore yields the same finite boundary; no new-node admission assumption is used to dereference it.
- `tg_reclaim_dead` is the only physical runtime unlink path (including terminal/orphan modes). `tg_reclaim_dead_admissible` rejects `worker_active != 0`, and the writer repeats this check after its metadata gate. Existing worker/reclaimer admission therefore preserves current TG bodies and non-self links throughout the invocation. Terminal/shutdown does not get an exception to that condition.

The cursor does not pin a TG after worker release. Its next invocation reacquires the existing worker/lifetime protocol and resolves the scalar ID against the current list before using a TG pointer. Logical DEAD can change while a body remains registered; the existing DEAD/internal flags and all owner-progress guards still decide whether to call it. The hint is never completion or reclamation authority.

## Explicit scope limits

This fixes traversal between TGs. It does not provide fairness among multiple quarantined arenas owned by the same TG: that owner's existing head/cursor traversal can still postpone a later same-TG arena while an earlier one retains a publisher. All of that work remains queued, and full collection can defer until the real owner advances. Do not call this a general wait-free collector or claim same-TG arena fairness without separate coverage/design.

A stable finite set of TGs retains its place across quota exhaustion, including quota 1. Arbitrary perpetual attach/detach remains subject to the existing lifecycle and scheduling constraints; an obsolete hint is a safe restart, not a stronger fairness guarantee. Multi-owner ID lookup may add a list walk, which must remain explicit in performance review.

Next validation must preserve the exact original function fixture; reuse successful publication/cancellation/automatic cases; exercise real tail-held constructors with zero/two workers, multiple blockers, quota 1/64 and detached hints. New scheduling can legitimately finish the independent arena during the initial nested call, so only the old rejected-candidate ordering precondition may be replaced by a record of earlier success. Actual nested deferral, retained raw owner, final independent progress, fixed bounds, and post-release completion must remain required.
