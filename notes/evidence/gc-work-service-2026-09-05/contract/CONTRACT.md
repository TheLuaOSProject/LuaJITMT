# Fair graph work-class selection: source contract

This review is limited to source scheduling on f9ec0a7217fc1a7eca61e17ab783bf32b9be1c61 and ROOT's exact v2 candidate. It implements nothing and executes no runtime. `base/` contains 225 committed inputs; `review-inputs/candidate/` contains only the two changed source files. Unchanged candidate inputs resolve to `base/`. The full candidate identity map and exact patch are copied beside those files.

## Recommendation and bound

Use ROOT's four-class common selector under the existing worker claim: published SSB, caller-owned active SSB, grey, recovery. Keep one persistent next-class scalar. Before attempting a class, publish its successor as the next choice. Attempt each class at most once in one circular pass, returning after the first nonzero legacy source unit or any defer. A zero result after four probes reports no source unit; it does not prove graph emptiness or authorize reclamation.

The conditional guarantee is precise: while an eligible class remains available and admitted selector calls continue, that class gets an attempt within four consecutive class positions and at most four quota-one selector calls. Empty classes can be skipped within a call. A retained or requeued head can end its quantum, but cannot retain first choice for the next quantum. This bound concerns class opportunities. It does not require that an attempt complete a payload.

The worker claim serializes every new scheduling-state access. Its acquire/release CAS and release store carry the relaxed scalar update into the next admitted claim (`lj_obj.h:5623`, `lj_gc2.c:1501,1576`). A failed claim, phase gate, native lease, active native frame, recorder veto or SWEEP finalizer veto never enters the selector and does not advance the hint. Fairness is conditional on those existing gates admitting work; the selector does not manufacture permission.

There is no new persistent pointer, item locator, reservation, allocation credit or root. The selector leaves the concrete ownership protocols inside the original leaf functions. No collector C local must survive claim release.

## Why all four callers/classes are needed

Baseline locations below refer to `base/src/lj_gc2.c`.

| Caller | Existing priority and starvation path | Required replacement scope |
| --- | --- | --- |
| Worker MARK/WEAK, 22555 | Published SSB followed by grey; recovery entered only if SSB moved zero and grey yielded no object. Ordinary continuously published strings can consume every quota while recovery remains PENDING. | Common selector, no active-TG argument. Preserve all outer claim, phase, native and defer checks. |
| Worker SWEEP, 22257 | Same recovery exclusion. It normally attempts grey after a successful SSB conversion, but a retained SSB slot defers before grey. | Common selector in the graph prefix only. Preserve recorder/finalizer vetoes, bridge/emptiness/grace/string/arena code verbatim. |
| Assist, 15038 | Grey can consume the entire quota; active SSB has priority over published SSB; recovery comes last. | Four distinct classes with the original `tg`. Preserve hard-limit, recursion, worker/assist claims and MARK/WEAK checks. |
| Bounded closure, 12047 | Published SSB can exhaust the full budget before active SSB, grey or recovery. This path also serves SWEEP preparation. | Common selector with the same `G2TG(g)` chosen by the old caller. Preserve NEEDSCAN stop and defer propagation. |

Three classes that combine active and published SSB leave assist's active-before-published starvation intact. A recovery-first alternate turn alone leaves a retained SSB head able to postpone grey on every remaining turn. Four independent classes give the simplest truthful quota-one argument. Preserving the old paired worker SSB-plus-grey unit would require a more complex schedule and a separate active/published fairness rule.

Production grey owner push and pop both use the deque bottom (`lj_gc2.c:11027,14296`): this is LIFO owner service. The baseline worker can consume a newly converted grey object ahead of older grey objects indefinitely. The new class schedule gives grey a turn but retains LIFO and retry-head behavior within grey. It does not prove fairness for each old grey object under an infinite stream of newer grey pushes. The same boundary applies to individual items within SSB and to the unchanged recovery directory/lane search.

A global active-SSB class also does not schedule TGs. Under adversarial interleaving it can be visited only on calls that lack a particular TG's private authority. The guarantee for active SSB is conditional on admitted calls carrying that owner. Existing owner flushes and FLUSH_SSB handshakes remain necessary for visibility and eventual service of remote/private suffixes.

## Ownership and leaf results

| Class | Exact original leaf | What one returned unit means | Refusal/lifetime requirement |
| --- | --- | --- | --- |
| Published SSB | `gc2_drain_published_ssb_to_grey(g,1)`, 14885 | One source slot consumed after mark/coalescing/terminal classification or durable transfer to grey/recovery. | Consumer count spans detached chains; failed admission retains the source slot; remainder is republished before consumer leave. Defer stops the selector. |
| Active SSB | `gc2_drain_active_ssb_to_grey(g,tg,1)`, 14947 | One owner-private source slot consumed and the exact cursor retreated after transfer/classification. | Same caller-selected live TG as before; never inspect another TG merely because global work exists. Failed admission retains the slot and cursor. |
| Grey | `gc2_drain_grey(g,1,stop)`, 22072 | DONE/STALE/REQUEUED count one; RETRY counts zero. DONE may be an owner NEEDSCAN handoff rather than completed payload traversal. | RETRY republishes its removed locator before defer; REQUEUED already transferred it. Both stop immediately, and no further class is attempted in that selector call. |
| Recovery | `gc2_recovery_drain_owned(g,1,stop)`, 21811 | One owned recovery attempt according to the existing main/small/Huge result convention. | Exact recovery count, CLAIMED/REDIRTY/PENDING transitions, SMR/body scopes and retry publication remain unchanged. Completion/failure return collisions preclude calling this count a completion receipt. |

The recovery leaf already rejects directory work when the exact item count is zero (`21771`); sticky `recovery_failed` is a close veto, not a drainable queue identity. There is no need for a second scheduling-level recovery directory implementation. The existing recovery lane quantum remains separate from the four graph classes.

The selector snapshots deferred_epoch for its one call, checks it after every leaf, and stops after a leaf-reported stop. Each outer drain retains its preexisting longer-lived epoch observation. This preserves a completed source prefix together with a later defer. Neither local hint advancement nor a nonzero legacy return cancels that refusal. In particular, worker logical progress remains zero if the outer defer epoch changed.

No native-entry or STOP/FINPAUSE policy is added here. Existing automatic-driver admission remains responsible for explicit control; existing active-phase worker/assist service retains its established semantics. STOP is not newly interpreted as a ban on every direct graph-drain API. The hint carries no cycle authority, so persisting it through phase/cycle changes, worker start/stop and explicit control changes is harmless. A new cycle still requires all its existing admission and reset protocols.

## Storage and accounting

Repurpose `uint32_t table_token_huge_pad` at `lj_obj.h:2000` as `graph_next_class`, with an explicit initial value zero in `lj_gc2_init`. The old field occurs only in its declaration and that initialization across all 225 inputs. Huge cursor setters write the adjacent node, incarnation and slot individually (`lj_gc2.c:19616`; `lj_obj.h:5375,5380`); they do not overwrite the new hint. The field keeps the same width/alignment and the following uint64_t counter positions. It should not be reset when an unrelated Huge cursor is reset. Source layout preservation is not a compiled ABI measurement.

The scheduling quota intentionally becomes stricter. Previously SWEEP could consume an SSB item and traverse a grey object for a single unit; MARK could perform both under limit one and clip its final return. The new selector charges one SSB source commit and one later grey attempt separately. Tests or cost models that assume both happen under one call need a separately reviewed quota-aware generation, with their original failures retained. This is not exact preservation of old scheduling counts.

Keep the existing public return forms and per-kind accounting roles: assist returns grey plus recovery plus weak, excluding SSB conversions; worker returns the capped aggregate scheduling count; bounded closure returns aggregate source units. `gc2_drain_grey` already increments the global grey counter, so the old worker's second global addition must be removed. Worker-specific grey accounting remains. SWEEP now also contributes grey units through that common helper, where the old inline SWEEP traversal did not increment the global grey counter. Neither counter is payload completion or allocation credit.

## Limits preserved

A bound on four leaf invocations is not a wall-time bound. Recovery directory search, payload traversal, retries within existing leaves, synchronous handshakes and pending-root full-chain flushes keep their existing costs. Unbounded `gc2_drain_ssb_owned` remains a separate priority/full-drain gap. The UINT_MAX weak bridge call through the bounded helper inherits fair class selection, but remains an effectively unbounded caller unless existing NEEDSCAN/defer conditions end it.

No queue ordering, recovery completion protocol, root EOF/READY certificate, same-TG arena rotation, detached-root continuation, string reclamation gate, native lease or no-worker scheduling-credit rule is changed. Fair graph-class opportunities alone do not establish full automatic-cycle completion or repair the preserved JIT-on worker-zero deficit.
