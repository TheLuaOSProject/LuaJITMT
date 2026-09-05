# ROOT source review: authoritative automatic-GC control

No source blocker was found in the frozen six-file candidate2 patch for its
bounded control/admission scope. This review approves the source design for
final combined validation; it does not certify the owner's still-running
matrix or recommend release. It performed no build or runtime test.

The exact full patch against aee88db569b82216b705408f00295a337a7393fe is
60b9c725d89b50daf06f89aab8582dffa62f84277f4fe23ec5fb780a18b568a5.
The incremental patch over the earlier STOP veto is
13f32a17375b92dcbc7d44bd2fc22a686505ae591cdd2ba3260f4fd582caf6a2.
Both patches, the six modified sources and source identities are copied here.
The integration HEAD at review is 0e2119e4; its subsequent changes are notes
and the independently repaired scheduler fixture, not GC runtime source.

## Public control and delayed threshold stores

The previous threshold bridge is a derived pacing state. A delayed first
attachment can store an already captured MAX after RESTART, and a published
live count can expose an old finite saved threshold after STOP. Checking a
counter twice or comparing the old threshold does not make either publication
authoritative. The earlier STOP-only veto fixes one direction but keeps lost
RESTART. Candidate2 removes both thresholds from the logical-running decision.

GCState's unused byte becomes auto_flags in the same position and width. Only
initialization clears the complete byte. STOP and RESTART use atomic fetch-OR
and fetch-AND on STOPPED alone. The existing atomic byte helpers implement
acquire/release RMWs; the readers use acquire loads. Current source contains no
attachment, detach, pacing, finalizer-threshold or cycle-reset store to the
control byte. Initialization occurs before the new global state is published.
No adjacent GCState field moves and no new allocation is required.

Public STOP publishes its bit before changing pacing. Public restart publishes
pacing before clearing that bit. Calls that overlap can order by their atomic
public-bit updates; a later derived-threshold store cannot change that order.
Completed-before-invocation STOP vetoes a later automatic invocation regardless
of first/last attachment, live-count ABA or delayed threshold writes. Restart
clears STOP without requiring an entrant to finish a threshold repair. The
existing debt trigger and pending-request entrance can drive work even if a
delayed store leaves the ordinary threshold at MAX.

api_gc_setlogical now receives the public operation's stopped parameter.
Inferring control from threshold == MAX would be incorrect: the ordinary
restart pause product can equal MAX numerically. Candidate1 still made that
mistake; candidate2 consistently selects STOP only for LUA_GCSTOP, and selects
restart for LUA_GCRESTART and a successful full-collect API return. The latter
keeps its established restart behavior even when the underlying collection
deferred. A throwing protected collect returns before the public control
update. Pacing/stepmul operations never change either control bit.

## Temporary finalizer suppression

FINPAUSE is an independent bit. Each actual callback sets it after state claim
and protected stack preparation and before the old threshold pause. The
existing physical finalizer owner spans this interval. Foreign owners cannot
dispatch a callback concurrently, while same-owner recursive dispatch retains
the active ownership count. Saving whether FINPAUSE was already set therefore
identifies nested scopes: only the outer callback clears its pause bit.

The critical operations around the protected callback preserve unwind:

- Failed state claim or stack preparation returns before setting FINPAUSE.
- Threshold stores and hook operations are nonthrowing. lj_trace_abort only
  requests asynchronous recorder abort. Profile dispatch update can wait for
  its existing update owner but does not throw through the new pause scope.
- lj_vm_pcall_unwind returns callback errors to the ordinary restore path.
  Native callback/reentry and nested explicit collection do not release the
  physical finalizer owner. Existing full-collect deferral while a finalizer
  is active remains unchanged.
- The caller restores current L, hook/profile state and thresholds, then
  clears only its owned FINPAUSE bit. This precedes potentially throwing
  ERRFIN delivery and the state-claim error cleanup.

An inner callback cannot clear its outer callback's pause. A STOP published by
a callback or a peer survives final pause removal. A callback RESTART or
successful/deferred full collect clears STOP but leaves FINPAUSE. The old
threshold restoration may still overwrite newer pacing values; it can no
longer overwrite these logical decisions. Physical owner coverage is essential
to this saved-bit design. Moving callback execution outside that owner will
require a different pause ownership protocol, not just retaining the bit.

ISRUNNING now reports false for STOPPED or FINPAUSE, independent of attachment
geometry. The prior MT threshold query could report true inside a temporarily
paused callback. The new result expresses the documented automatic suppression
consistently; this visible correction must be explicit in the integration
notes and actual callback controls. It is not a promise that active collector
work has stopped or that an explicit STEP/COLLECT request is prohibited.

## Admission and unchanged progress limits

The new pending predicate requires a nonzero durable request, IDLE and zero
control flags. Its C callers already have a safe Lua frame. The VM branch
checks the same state before using the existing slow GC target; it adds no
stack layout or temporary-root change. Existing worker/phase/root admission
still determines who may start MARK, including rejection of the non-owner
GCSCAN token. A nonzero leader is not a new ownership certificate.

The actual automatic driver independently samples logical running before
publishing/consuming automatic work. Raw allocation request creation checks
the same control state. Old threshold and hard-cadence branches may enter a
slow helper while stopped, but cannot use the new IDLE request entrance to
bypass this driver decision. Already-active assists/workers and explicit
STEP/COLLECT retain their old authority. Native GC threshold/hard-cadence
geometry is unchanged; new interpreter admission evidence is not proof that
every continuously running native allocation loop completes collection.

The driver samples control once per invocation. STOP or FINPAUSE is not a
cancellation barrier for already-authorized work, nor an atomic transaction
with MARK publication. The existing automatic driver can wait for roots,
phase ownership or peers. FINPAUSE still makes automatic admission depend on
an active finalizer returning. This source fixes logical control and safe
request admission, not asynchronous collection or the full lockless goal.

## Required final evidence

Keep the unchanged real first-attach STOP and lost-RESTART schedules, numerical
MAX control, ordinary request entrances, active STOP, first/last child, real
native return and nested/peer/throwing finalizer controls. Verify STOP/restart
inside callbacks, callback-owner depth and hook/flag restoration in normal,
assertion and ASan builds with exact matching input/fixture flags. Preserve
candidate1's numeric-MAX failure and the earlier veto's lost-RESTART failures.

Validate the final source with stock/JIT/FFI interactions and the repaired
scheduler fixture before integration. Retain separately the existing hard
assist/accounting failures, closure-construction wait and configured-worker
SWEEP completion bound; do not relabel them as passes. The fixed scheduler's
matched publication diagnosis does not resolve any of those other outcomes.
No concurrent string-reclamation or release-readiness claim follows here.
