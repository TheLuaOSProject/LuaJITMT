# Owned service receipt map

Base: `f9ec0a7217fc1a7eca61e17ab783bf32b9be1c61`, 225 committed runtime/generator inputs copied into `base/`. `src/lj_gc2.c` SHA256 is `3571a3f2128114c475a730fcb35a413cffdfd27124ba2dcb9e136d9690edbea9`. This is a source audit, with no runtime edits, builds, debugger runs or runtime validation.

All line references below are to that copied source. “Completion” must identify what completed. A consumed SSB slot, a returned payload handler, a retired recovery count and closure of the graph are four different facts. This map does not assign allocation credit to any of them.

## Receipt vocabulary and ownership

- **Source commit:** the input SSB slot/cursor is consumed, or the exact recovery incarnation is removed and its count discharged. This is a concrete committed transition, independent of whether successor work remains.
- **Transfer:** the source is discharged only because its current work is represented elsewhere: grey, recovery, a thread NEEDSCAN token or a table rescan publication.
- **Retry/requeue:** the current work remains or is republished. A recovery REDIRTY→PENDING transition retains the same aggregate count. A grey pop followed by push retires one queue position but does not finish that work.
- **Payload outcome:** a pass completed according to that handler, a prior exact proof covered it, a stale/invalid body was skipped, ownership was delegated, or the pass needs replay. It is not a transitive graph-closure certificate.
- **Failure veto:** a branch sets sticky fail-closed state. This can coexist with an already-committed slot or already-published recovery identity. It cannot always be represented as an exclusive alternative to progress.
- **Deferral:** the invocation must retain its existing stop/yield behavior. It can coexist with committed prefix facts. An observed global `deferred_epoch` change is not necessarily attributable to one local item.

Worker-owned graph calls run beneath `worker_active`, except explicit helper paths that first acquire the same token and the established owner-private SSB access. A receipt is scalar data produced under that authority. It must contain no object/TG/arena/slot pointer used after release. It is not a transferable claim or permission to reclaim, clear a scheduling obligation, or borrow another TG's roots.

## SSB publication and source-slot commit

| Function/branch | Existing result and concrete effect | Sound receipt |
| --- | --- | --- |
| `gc2_grey_push`, 11027 | Returns 1 after slot release-store, then bottom release-store. Failure returns 0. | Successful destination publication to grey. Does not scan the payload. |
| `gc2_publish_worker`, 14698 | Short-circuit grey push; otherwise recovery publication; either success returns 1. Both fail: sticky failure, return 0. | GREY destination, RECOVERY_ACCEPTED with its detailed publication outcome, or failure. The current boolean loses destination details. |
| `gc2_ssb_mark_one`, 14706: main-thread identity | Routes to `gc2_publish_worker`, returns that boolean. | Transfer/accepted successor, not a stack scan. |
| Preservation returns DEAD with `retry=1` | Returns 0 after releasing the scope. | Input slot must remain intact; transient lifetime refusal. |
| Preservation returns DEAD with `retry=0` | Returns 1. | Source may be consumed as stale/nontraversable under the existing admission result. No payload-completion claim. |
| Table current-cycle/coalescible proof, clear NEEDSCAN, exact rescan token NONE | Returns 1 inside retained body scope. | Already-covered source request. This is not new traversal. Keep all three existing observations. |
| Traversable object/userdata | Calls `gc2_publish_worker`, then releases scope, returns its boolean. | Successor publication/coverage permits source consumption. No payload scan here. |
| Leaf or NULL | Returns 1. | Leaf/empty-source discharge. |
| `gc2_drain_published_ssb_to_grey`, 14887: helper false | Increments defer epoch; leaves current slot/count unchanged. Republishes the detached remainder before consumer leave. | Retained current slot, plus any earlier committed prefix; local deferral. |
| Same drain: helper true | Clears slot, decrements and release-publishes node count, adjusts remembered count, increments `nitems`. Node recycling follows only at count zero. | Increment **SSB source commit** only after slot/count commit, carrying that helper's disposition. Never increment it at the pre-clear test hook. |
| `gc2_drain_active_ssb_to_grey`, 14946 | Equivalent semantics for proven owner-private suffix: slot clear precedes release-published cursor retreat. False leaves the suffix at the current item. | Source commit at cursor retreat; same disposition/prefix rules. Do not gain new permission to access a remote suffix. |

The published consumer can keep a chain in C locals only while its counted consumer ownership remains active. The existing remainder publication and `gc2_ssb_consumer_leave` must finish before the receipt escapes. A receipt does not replace that durable continuation.

### Recovery publication success is itself a union

`gc2_recovery_publish` (10675) → scoped lookup (10608) → main/small/huge publication. The success boolean authorizes the existing source handoff, but does not mean “one new recovery item.”

| Publication leaf | Success branches | Failure/qualifier |
| --- | --- | --- |
| Main, 10346 | Existing PENDING/REDIRTY; CLAIMED→REDIRTY; reserved count then IDLE→PENDING. | Only the last branch creates a new count. Failed reserve/invalid state is not publication. |
| Small, 10443 | Existing PENDING/REDIRTY; CLAIMED→REDIRTY; another counted RESCUE/RECOVERY publisher before side-plane publication; late DESTRUCT terminal intent; newly reserved IDLE→PENDING. | “Other counted publisher” is durable work before a PENDING bit appears. MUTATING alone is not coalescible. Late-free success may create no recovery identity. A successful PENDING CAS can be followed by lifetime-restore failure: publication and fail-closed state then coexist. |
| Huge, 10559 | Existing PENDING/REDIRTY; CLAIMED→REDIRTY; already FREEING/DEFER_FREE graph role; newly reserved IDLE→PENDING. | Terminal success creates no recovery count. Mapping relocation is result 0 and permits the existing bounded lookup retry; persistent rejection is negative and becomes failure at the scoped wrapper. |

Exact “new/existing/redirtied/terminal” receipts must be selected at these existing branches and passed out as scalars. They cannot be reconstructed by re-reading the object's state after scope release or by subtracting global publication counters.

## Grey traversal and payload outcome

`gc2_traverse_obj` (21315) currently returns RETRY=0, DONE=1, STALE=2, REQUEUED=3. These four values are not four payload-completion states.

| Path | Existing return | Exact interpretation |
| --- | --- | --- |
| Scoped preservation rejects transiently | RETRY | Caller still owes a durable replacement for the popped locator. No payload traversal. |
| Scoped preservation rejects stale identity | STALE | Existing caller consumes the queue position without payload traversal. |
| Unexpected/non-GC type after retained admission | DONE | Handler takes its `out` path; distinguish skipped type from visited payload. |
| Table helper STABLE (`gc2_traverse_tab_rec`, 18979) | DONE | The table handler finished its selected legacy pass. It may have published descendants/weak work. Legacy unstamped completion and current stamped proof are different strengths; neither is global graph closure. |
| Table RETRY/REQUEUED | REQUEUED | Legacy helper or `gc2_table_rescan_requeue_held` establishes retry/close-veto state before scope release. Partial marks remain; the pass must be replayed. |
| Table INVALID contract | REQUEUED | Republishes, sets fail-closed and defers. “REQUEUED” alone does not prove a healthy successor locator. |
| Thread or embedded main-thread helper returns 1 | REQUEUED | Explicit requeue path, possibly with a failure veto; see next table. |
| Thread helper returns 0 | DONE | Can mean actual scan, already-covered owner scan, live-owner NEEDSCAN handoff, or invalid-geometry skip. Must split these source branches. |
| FUNC/UPVAL/UDATA/TRACE; valid PROTO | DONE | A void handler returned. It can publish descendants, skip invalid geometry, skip an already-covered prototype, or retain work via deeper helpers. Do not promote the return to complete graph traversal. |
| Invalid PROTO geometry | DONE | No prototype traversal ran. |
| STR/CDATA leaves | DONE | Leaf handler discharge. |

The table handler already reports RETRY/STABLE/REQUEUED/INVALID; its requeue helpers discard publication return values. To distinguish an established successor from a failure-veto path, an optional detail result must retain those existing publication outcomes. It must not add a second publication or change table token settlement.

### Thread traversal: the important DONE collision

`gc2_traverse_thread` starts at 21099. Its return value means “explicit requeue path taken,” not “scan incomplete.”

| Thread path | Current return | Receipt classification |
| --- | --- | --- |
| `gc2_thread_owner_scans` proves matching cycle, handoff, dirty epoch and clear NEEDSCAN | 0 | Already covered by an owner scan; no new stack scan here. |
| Busy live owner; `gc2_thread_set_needscan`; post-publication live-owner recheck succeeds | 0 | **Transfer to authoritative NEEDSCAN work**, possibly with recovery publication in its release-race closure. Not payload completion. |
| Geometry validation fails under retained authority | 0 | Skipped geometry, not successful stack coverage. |
| Full pass with no `stack_retry`; publishes cycle/dirty stamp and successfully clears NEEDSCAN | 0 | This handler completed its stack pass. Child graph work can remain. |
| SMR/claim/release race, stack retry, or NEEDSCAN clear failure | 1 | Releases stack claim; publishes concrete retry or pins fail-closed; increments defer before ending its registry lease. |

`gc2_thread_set_needscan` (8063) publishes INSTALLING before the header hint, then establishes COUNTED and the handoff. The same-value owner CAS/recovery fallback closes owner-release races. A result refactor must preserve that entire protocol and take its result before dropping the scope. It must not classify a grey removal as a finished scan merely because `gc2_traverse_thread` returned zero.

### Grey source retirement and partial work

`gc2_drain_grey` (22073), the inline SWEEP loop (22274+) and MARK/WEAK loop (22557+) pop a grey item before traversing it.

- RETRY: republish the popped identity, update existing marks/wake/defer state, then stop. The legacy `n` does **not** include this pop. A receipt should still record that a pop/republication occurred, without claiming completed work.
- REQUEUED: the traversal already transferred to a locator or close veto. The caller stops and legacy `n` **does** include this item. Its completion classification is still transfer/retry/failure, not DONE.
- DONE or STALE: legacy `n` includes the item. Preserve the more precise payload detail above rather than naming every such item “payload completed.”

In the SWEEP loop, one iteration may consume an SSB slot, pop the grey object just published from it, and traverse that object, yet increase legacy `n` only once. Conversely, an SSB commit followed by a grey RETRY can return zero legacy units. Independent receipt counters are event counts; their sum is not the existing scheduling budget.

## Recovery completion primitives

| Function | Actual retirement | Retained incarnation | Failure collision |
| --- | --- | --- | --- |
| `gc2_recovery_complete_main`, 21429 | CLAIMED→IDLE CAS, then exact aggregate count decrement; returns 0. | REDIRTY→PENDING, same count; returns 1. | Invalid/lost state pins failure and also returns 0. |
| `gc2_recovery_complete_small`, 21449 | CLAIMED→IDLE CAS, exact count decrement, completion wake; returns 0. | REDIRTY→PENDING, same count; returns 1. | Failed alternatives pin failure and also return 0. |
| `gc2_recovery_complete_huge`, 21499 | Uses arena LIVE/SWEEP/UNMAP result, discharges huge and total count; returns 0. | Arena REQUEUED or wrapper REDIRTY→PENDING fallback; returns 1. | LOST plus failed fallback pins failure and also returns 0. |
| `gc2_recovery_retry_small/huge`, 21467/21482 | No count retirement. | CLAIMED or REDIRTY→PENDING; already PENDING is accepted. Both are void. | Other state pins failure. |

The `0` collision is decisive: `!gc2_recovery_complete_small(...)` is not a completion receipt. The new internal result must distinguish retirement, retained PENDING and failure at those branches while keeping legacy boolean mappings unchanged.

After a successful recovery-state retirement, `gc2_recovery_count_complete` (21393) decrements the exact count and increments `recovery_drained`. Underflow is terminal `abort`, with no valid returning receipt. Huge decrements its subcount before the total count. A receipt is committed only after both required bookkeeping operations return.

### Huge arena return contract

`lj_arena_hugetab_recovery_complete` (`lj_arena.c:2967`; public constants in `lj_arena.h:716`) already has a useful typed result:

- LIVE: authoritative recovery bits were cleared. This includes a deferred free still waiting for admitted readers; it is not “object stayed live” or “payload completed.”
- SWEEP: authoritative recovery bits were cleared and deferred-free state transferred to physical sweep. GC2 then requests grace. This is recovery retirement plus later physical work.
- REQUEUED: root ownership or BUSY retains the same count as PENDING.
- LOST: no completed transition; wrapper may still perform the existing REDIRTY→PENDING fallback.
- UNMAP: the GC2 wrapper retains a compatibility branch for this named result, but the current normal arena completion function has **no return producing it**. Do not report it as an observed current path. Keep the branch unchanged in a refactor.

Fill receipt scalars from the existing result/`LJHugeInfo` while valid. Never inspect `base` after the existing unmap call or introduce a new unmap. No arena API change is needed merely to preserve its result instead of flattening it.

## Main, small and Huge recovery drain calls

| Call/branch | Legacy result | Source/recovery/payload receipt |
| --- | --- | --- |
| `gc2_recovery_drain_main_one`, 21530: PENDING claim loses | 0 | No claimed incarnation; not proof the lane is empty. |
| Main claim wins | 1 | Pair the thread payload detail with exact completion result. Thread requeue OR retained completion sets legacy stop. A retired main recovery count can coexist with NEEDSCAN handoff or an explicit successor. |
| `gc2_recovery_drain_small_one`, 21551: registry/SMR absent or complete search finds no admitted claim | 0 | No completed item. Search can have seen retained identities, lost claims or a failure veto. Preserve those observations separately from empty. |
| Small preclaim: lifetime LIVE/CONSTRUCT→MUTATING loses | Continues search | No recovery claim or completion. Do not attribute a changed cursor/scan attempt as service. |
| Small exact-ready fails | Restores lifetime, pins failure, continues | Failure observation; no recovery retirement. Restore itself can also fail. |
| Small late + constructor/root ownership before claim | Restores lifetime, continues | Same PENDING identity retained for semantic owner; no completion. |
| Small PENDING→CLAIMED loses | Restores lifetime, continues | No claimed recovery item; preserve restore failure if any. |
| Small claim wins | Cursor advances before payload handling | A claim/cursor fact only. It is not completion. |
| Late after claim, still semantically retained | Lifetime restore then retry to PENDING; returns 1 | Same recovery count retained. Legacy stop is set. |
| Late after claim, ordinary unrooted LIVE | Lifetime restore then completion helper; returns 1 | May retire without traversing, requeue REDIRTY, or fail. Classify using the typed completion result. |
| Postclaim lifetime restore fails | Returns 1 | Failure; no permission to claim payload or recovery completion. |
| Traversal RETRY + late | Calls completion helper, always sets stop, returns 1 | May actually retire this recovery incarnation despite no payload pass. Keep both facts. |
| Traversal RETRY without late | Retry to PENDING, stop, return 1 | Same incarnation retained, no recovery-count retirement. |
| Traversal REQUEUED | Calls completion helper, always stops, returns 1 | **Transfer + possible recovery retirement + deferral** is legal. It is not an exclusive “complete OR deferred” result. |
| Traversal DONE/STALE | Completion helper, return 1; stop only if retained result | Pair exact payload detail with retirement/retained/failure result. |
| `gc2_recovery_drain_hugetab_range`, 21695: PENDING CAS wins | At most one claimed item per call | Skip traversal for CDATA leaf; otherwise use exact traversal detail. RETRY invokes retry helper; other results invoke completion helper, including REQUEUED. Cursor advances; requeue sets stop. |
| `gc2_recovery_drain_huge_one`, 21738 | Holds registry SMR through search/range call; returns 1 for one attempted claim | Preserve that same scope and table lifetime. Result zero can be admission/search failure; it is not proof no pending work exists. |
| `gc2_recovery_drain_one`, 21771 | Chooses main/small/huge with the existing lane quantum; returns 1 for one attempted claim | Lane selection and credit remain unchanged. A result receipt may identify the lane and its outcome, not change fairness. |
| `gc2_recovery_drain_owned`, 21812 | Increments `n` after any successful drain-one attempt; `stop_one` increments global defer and ends batch | Preserve all earlier retirement/transfer facts, plus the last attempted item's disposition and stop. Do not rename `n` to completed recoveries. |

Small recovery source lookup may make a full wrapped registry/cell pass; Huge lookup may scan directory-sized lanes. This result refactor does not make one attempted item a fixed wall-time unit.

## Partial progress before deferral: exact source schedules

These are source-derived branch compositions, **not new runtime tests**:

1. Two SSB slots commit, third preservation returns RETRY. The receipt contains two committed slots with their dispositions, plus one retained slot and local defer. Legacy `nitems=2`; worker `progress` can subsequently become zero. The two facts must survive.
2. SWEEP consumes one SSB slot and then pops a grey item whose traversal returns RETRY. The original source slot is already committed; the grey identity is republished; legacy SWEEP `n` can remain zero. The receipt must not erase the source commit or call the retry completed traversal.
3. Small recovery traverses a table, which republishes a rescan and returns REQUEUED; completion then wins CLAIMED→IDLE and discharges the recovery count. Report one recovery retirement and one payload transfer, with defer set. A zero-progress summary loses the retirement; a completion-only summary loses the successor.
4. A completed payload pass races REDIRTY. Completion restores PENDING and retains the count. Report the handler pass and retained recovery; no recovery retirement. Later publication has not been paid by this pass.
5. A recovery completion helper reaches its fail-closed branch and returns zero. The legacy caller may return one attempted item without setting its stop flag. Report failure and no recovery retirement. Preserve that legacy scheduling behavior in the result-only refactor; do not silently fix it here.
6. Busy thread consumes a grey position by establishing NEEDSCAN and returns DONE through the existing adapter. Report source retirement plus owner transfer, with no completed stack pass. No extra duplicate grey publication is authorized.

## Aggregation and caller map

The receipt is appended at the leaf commit/return branch and survives later defer. The existing `progress` output at `gc2_worker_drain_inner:22616` can keep its current zero-on-defer compatibility behavior. New receipt fields must not use that zeroed value as their source.

| Caller | Current authority/return contract | Result-only treatment |
| --- | --- | --- |
| `gc2_worker_sweep_progress`, 22257 | Existing worker claim; mixed graph, suffix flush, grace and physical work | Accumulate graph leaf receipts independently. Label other progress separately; leave priority, limits and returns unchanged. |
| `gc2_worker_drain_inner`, 22444 | Preclaim finalizer splice; then claim/phase/gate/native checks and one quantum | Receipt phase/cycle sampled under actual claim. Refusal has no graph facts. Commit receipt before gate reopen/release. Finalizer preclaim work cannot share graph ownership. |
| `gc2_worker_drain_logical`, 22640; worker main/public drain | Legacy progress/attempt counts and current backoff | Old callers pass NULL or ignore the new side result. No new scheduling decision. |
| `gc2_mark_drain_owned_bounded`, 12047; `gc2_drain_ssb_owned`, 14986 | Existing bounded/full owner drains; the latter returns converted slots | Keep old wrappers and returns. A future receipt-aware caller may pass one accumulator; no graph-completion inference from the converted return. |
| `gc2_recycle_published_ssb_for_flush`, 14407 | Claims worker; MARK/WEAK conversion only | Keep NULL receipt unless explicitly requested; do not broaden into traversal. |
| `lj_gc2_assist`, 15038 | MARK/WEAK worker claim with assist scope | Preserve guards, flags, limits, source order and old return. |
| `lj_gc2_test_recovery_drain`, 15239 | Takes worker claim, returns attempted count | Keep compatibility. A later independent fixture can inspect an added test-only receipt sibling. No test change in this audit. |
| `lj_gc2_step_explicit`, 3116; automatic C driver | Public completion-only result; multiple ownership intervals/callbacks | Unchanged in the smallest leaf-result refactor. A later consumer must not merge old/new-cycle receipts across callbacks or use a released receipt as ownership. |

No leaf result by itself proves graph emptiness, pays an allocation threshold, advances native admission, or cancels an automatic request. Work-class scheduling is a separate ROOT audit.
