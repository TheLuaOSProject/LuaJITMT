# Smallest internal result refactor

This is a reviewable source contract, not an implemented or validated runtime patch. Its purpose is to expose existing leaf outcomes without changing any queue action, claim, native gate, scheduler branch or public step result. The source map is in `RECEIPT-MAP.md`.

## Shape

Keep the change inside `lj_gc2.c`. The Huge arena API already exposes the needed completion alternatives, so `lj_arena.c/h` do not need alteration. No new global/TG field, TLS accumulator, runtime allocation, generated VM predicate, GC-control field or public API is required.

Use an optional, caller-owned scalar side result alongside the existing return values. Conceptually:

```c
/* Proposed internal interfaces, not compiled into base/. */
typedef struct GC2LeafReceipt GC2LeafReceipt;
typedef struct GC2QuantumReceipt GC2QuantumReceipt;

static int gc2_ssb_mark_one_result(global_State *, GCobj *,
                                 GC2LeafReceipt *);
static int gc2_traverse_obj_result(global_State *, GCobj *,
                                 GC2LeafReceipt *);
static int gc2_recovery_complete_small_result(global_State *, GCArena *,
                                              uint32_t, GC2LeafReceipt *);
/* Main/Huge completion and retry siblings have the same optional detail. */

static uint32_t gc2_drain_published_ssb_to_grey_result(
  global_State *, uint32_t, GC2QuantumReceipt *);
static uint32_t gc2_drain_grey_result(
  global_State *, uint32_t, int *, GC2QuantumReceipt *);
static uint32_t gc2_recovery_drain_owned_result(
  global_State *, uint32_t, int *, GC2QuantumReceipt *);
```

The existing functions remain thin wrappers with NULL detail, or their existing internal callers acquire a final optional parameter. Prefer wrappers where this avoids editing unrelated root/assist/public callers. The returned integer and the existing stop-pointer writes keep **exactly** their current meanings, including existing failure collisions. New result fields are facts only; no old loop reads them in this refactor.

A leaf result needs independent dimensions, rather than a single COMPLETE/DEFERRED enum:

| Dimension | Minimum distinctions |
| --- | --- |
| Source | None claimed; SSB slot committed; grey popped/discharged; recovery incarnation claimed; recovery count retired; current source retained or republished. |
| Payload | Not visited; leaf/stale/invalid skip; already-covered proof; handler/pass returned; owner NEEDSCAN transfer; explicit replay/requeue. |
| Successor | Grey; recovery accepted; exact new/existing/redirtied recovery when known; table/owner token; terminal free coverage; no successor needed; unclassified accepted fallback. |
| Exceptional state | Local retry/defer; local fail-closed branch; independently observed global defer/veto. These can coexist with committed facts. |

The quantum aggregates only scalar counts/flags from its own calls, with phase/cycle sampled after its real worker claim. It preserves committed prefixes even when the invocation later stops. It must not count attempted table edges as completed graph jobs, and the sum of its event counters is not automatically a scheduling budget. In particular, one SWEEP legacy unit may contain both an SSB commit and a grey discharge.

A concrete compact leaf layout for review is four scalar fields: `source_events` (bitset), `payload_kind` (one classified handler outcome), `successors` (bitset), and `local_flags` (bitset). Source events include SSB_COMMITTED, GREY_POPPED, RECOVERY_CLAIMED, RECOVERY_RETIRED and SOURCE_RETAINED/REPUBLISHED. Successors can coexist: for example a table rescan publication plus a retained REDIRTY recovery incarnation. The containing caller already knows the source lane. The quantum keeps separate per-lane event counts and observed flags, with checked/saturating bookkeeping if used by an unbounded compatibility wrapper. No event count is an implicit byte credit.

Append the leaf result only once at the caller's established source commit/retention boundary; nested publication merely fills its scalar disposition. Independent SSB and grey source commits in one loop iteration are two leaf events even if old loop accounting calls them one unit. A small-lane scan can set a failure/refusal observation without claiming any item; append its flags without inventing a source event.

## Necessary leaf changes, in order

1. **Preserve recovery completion outcomes at their branches.** Add a completion-detail enum with `RETIRED_LIVE`, `RETIRED_TO_SWEEP`, compatibility `RETIRED_UNMAP`, `PENDING_AGAIN`, and `FAILED`. Main/small ordinary retirement use RETIRED_LIVE as a recovery-state label, not an object-liveness assertion. Record retirement only after the existing exact count bookkeeping returns. The old return remains `PENDING_AGAIN ? 1 : 0`, including zero for FAILED. Huge retains the arena result before mapping it to the old boolean; its failed REDIRTY fallback is FAILED. No additional state read or CAS is needed.
2. **Expose retry completion and lifetime-restore failures.** Retry helpers report newly restored PENDING, already PENDING, or failure, without changing their existing transitions. Small drain preserves the return of its lifetime-release calls for the receipt, including branches that currently ignore it, without altering those branches. A postclaim restore failure cannot create a successful payload or retirement receipt.
3. **Separate traversal return from traversal detail.** Keep RETRY/DONE/STALE/REQUEUED numeric behavior. Add detail at each existing branch. Thread traversal must report already-covered owner scan, actual pass, NEEDSCAN handoff, geometry skip and explicit requeue separately. Table traversal forwards its existing result and publication/failure detail. Void FUNC/UPVAL/UDATA/TRACE handlers remain `HANDLER_RETURNED/DETAIL_UNAVAILABLE` unless their existing early-return branches receive explicit scalar detail; do not fabricate a complete payload proof from their void return. That conservative label is sufficient for a first result refactor.
4. **Expose SSB source disposition before consumption.** `gc2_ssb_mark_one_result` returns the old success bit plus leaf/stale/covered/grey/recovery disposition. `gc2_publish_worker` retains which short-circuit arm succeeded. For the smallest first patch, a recovery fallback may honestly be labeled `RECOVERY_ACCEPTED` (the union of durable handoff and terminal coverage), **not** `NEW_RECOVERY` or even unconditionally `TRANSFER`. Exact new/existing/terminal classification requires optional detail propagated through the main/small/huge publication branches listed in the map; that is a bounded follow-on within the same file, without changing barrier behavior.
5. **Append only at source commits.** The SSB drain appends the leaf disposition after slot/count or cursor commit. Grey consumers append the pop/disposition after the existing retry publication or handler cleanup. Recovery drain pairs payload detail with exact completion detail before releasing its existing SMR scope; then it appends scalar facts. The last grey/small/Huge body must not be inspected again merely to complete the receipt.
6. **Carry the prefix through the existing call stack.** Give SWEEP progress and `gc2_worker_drain_inner` an optional result path. Existing graph leaves receive the accumulator; private-suffix flush, grace, arena and preclaim finalizer work receive separate labels or no graph counts. Finalize the scalar result before native gate reopen and worker release. Existing `progress=0` on changed defer epoch stays unchanged, while the separate receipt keeps actual prefix facts.

These steps expose an exact **source/recovery retirement contract** even while some payload handlers remain opaque. They do not establish complete graph-service or cost accounting. Extending every void child-marking helper merely to claim universal payload completion would be a substantially broader change and is unnecessary for an honest first receipt.

## Required mechanical invariants

- Do not replace existing booleans with a differently truthy enum at an old call site. In particular, FAILED currently maps to zero in recovery completion, and a void retry currently changes control only through its existing stop/defer writes. Keep that behavior during this refactor.
- Do not call a leaf twice to obtain its detail, re-read a freed body, or use global counter deltas. Select the scalar detail at the existing successful/failed branch.
- A result can contain both `recovery_retired=1` and `payload_requeued=1`, or both a committed prefix and `deferred=1`. Do not make those states mutually exclusive and do not zero the receipt on defer.
- A failed later publication does not roll back an earlier SSB commit or recovery count retirement. Keep existing fail-closed and continuation behavior; the result records it.
- Keep existing consumer refs, scope leave, lifetime restoration, node recycling, count-underflow abort, Huge free/grace actions, wake calls and phase checks in their original order. A receipt owns no locator.
- Keep the three existing grey-consumption sites consistent. Extracting them into a new scheduler loop is unnecessary; use a shared scalar classification helper if helpful while retaining pop/service order and legacy accounting at each site.
- Local receipt flags describe this call. A sampled global veto/defer flag is labeled as observed; it is not evidence that this item installed it. Other publishers can change these global fields.
- Leave worker drain priority, lane selection, limits, public explicit-step semantics, automatic control and all JIT entry/reopen decisions unchanged. This contract grants no allocation credit.

## Review boundary and later validation

This contract is ready for review at the leaf/result level. No runtime patch is attached: ROOT requested source contract review before runtime validation, and the broader scheduling policy remains separate. A later implementation can be one isolated `lj_gc2.c` result-only patch, with old entry wrappers preserved.

After source approval, meaningful controls would force: SSB committed-prefix then retained slot; grey transfer versus completed/stale handler; thread NEEDSCAN handoff; small/Huge REDIRTY versus real count retirement; small postclaim late/free and constructor ownership; Huge deferred free with root/BUSY/readers; completion failure; and successful recovery retirement followed by payload requeue/defer. Oracles should compare old returns and queue/counter states as well as receipt fields. No such tests were added or run here.
