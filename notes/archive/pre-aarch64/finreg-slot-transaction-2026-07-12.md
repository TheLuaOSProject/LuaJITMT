# FINREG exact-slot transaction (2026-07-12)

## Implemented linearization rules

- `FINCLAIM` is the exclusive transaction state for one keyed callback slot.
  Every replacement of that state is an exact keyed CAS while the table body
  lease and tactical SMR admission remain held. An impossible store/rollback
  mismatch is fail-stop; it never disables a generation or drops a callback.
- A clear which cannot find the key is a true no-op. A repeated clear of a
  stable nil slot with no `LJ_GC_CDATA_FIN` bit is also a no-op.
- Clear claims the current slot, completely retires every ordered membership,
  clears the FIN bit/notification, and only then publishes nil. A re-enable
  cannot observe nil before the old registration is fully retired.
- Ordered retirement cannot return a rollbackable result after committing one
  node. If an adjacent-node/body admission changes after a partial commit, the
  owner retains `FINCLAIM` and restarts from the current head until the target
  membership is completely retired.
- Replacing a nonnil callback is one transaction: weak-key barrier, complete
  retirement of the prior membership, publication of exactly one replacement
  order node, and exact callback publication. It preserves the FIN bit and does
  not increment `finreg_cdata_sets`. Repeating the identical callback reuses the
  existing membership and has no telemetry or order-node churn.
- Ordinary contention no longer escapes to Lua as `thread busy`; the operation
  retries with system-wide lock-free progress.

## Recursive finalizers

`gc2_call_finalizer()` retains the finalized cdata body scope throughout its
Lua callback. A same-owner recursive `collectgarbage("collect")` could therefore
reach SWEEP and wait forever for the arena reader held by its own outer frame.
`lj_gc2_collect_active()` now recognizes that exact current finalizer owner and
defers before requesting or advancing another major cycle. The outer collector
resumes after the callback drops its scope.

## Deterministic coverage

`tests/t-ffi-finreg-clear-races.c` pauses and verifies:

- direct replacement and identical re-registration;
- replacement versus clear while replacement owns `FINCLAIM`;
- enable order publication versus clear retry;
- an authoritative clear MISS versus a later enable;
- clear after FIN/order retirement but before nil versus re-enable retry;
- same-owner recursive full collection from a cdata finalizer.

Each stable boundary checks the exact callback slot, active order-node count,
FIN flag, GC2 pending result, and set/clear notification deltas.

## Remaining generation debt

P0/P1 follow-up: GC2 P_WEAK/close discovery currently publishes nil and queues a
preclaimed callback while leaving `LJ_GC_CDATA_FIN` as the later suppression
latch. A concurrent re-enable can publish a new callback/order before the old
queued callback dispatches; `gc2_finreg_cdata_dispatch_claim_preclaimed()` may
then clear the new registration's FIN bit. The queued record needs an exact
per-registration generation/sentinel protocol so an old dispatch can suppress
or consume only its own generation and never mutate a later enable.

Performance/nonblocking follow-up: retry paths still use scheduler yields, and
ordered-retirement completion retains the transaction's tactical SMR admission
while retrying after a partial commit. This is safe and does not expose a
partially retired callback, but should ultimately use an owner-tagged resumable
claim or a commit path that cannot need adjacent-node admission.
