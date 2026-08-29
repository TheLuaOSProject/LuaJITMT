# GC2 table-token exact-target correction (2026-07-18)

## Status

This corrects the generation protocol in
`gc2-table-rescan-helpable-token-design-2026-07-17.md` before the
dormant descriptor/token plane is enabled in production. The descriptor and
side-token storage remain dormant at this checkpoint; the legacy table rescan
state/count is still the live authority. No file under `plan/` was changed.

## Defect in locally incremented token generations

The earlier design let every helper independently advance a table-local token
generation. That is not idempotent for a single global descriptor request.
This schedule can recreate already-completed work after its locator vanished:

1. the descriptor publishes `ACTIVE(table, D)`;
2. helpers A and B both exact-recheck that value;
3. A refreshes the token, clears the descriptor, and a scanner completes the
   refreshed token back to `NONE`;
4. delayed B resumes and locally refreshes that `NONE` token again; and
5. no descriptor remains to advance the global requested generation or make a
   close-pass acknowledgement notice B's late publication.

The token is physically visible, but an epoch acknowledgement keyed to `D`
may already have completed. A close racing step 4 could therefore miss the
late `PENDING` state. The earlier claim that multiple helpers may each refresh
one descriptor request was incorrect.

## Correct exact-target transitions

The global descriptor generation is now the exact per-table token generation
for that request. For an exact `ACTIVE(table, D)` ticket, transfer uses:

```text
NONE(g),    g < D  -> PENDING(D)
PENDING(g), g < D  -> PENDING(D)
PENDING(D)         -> PENDING(D)  already transferred
NONE(D)            -> NONE(D)     already completed
state with g > D   -> unchanged   stale helper
PENDING(D)         -> NONE(D)     exact stable-scan completion
```

Transfer and completion never increment a table-local generation. Only a new
global descriptor generation creates a new token generation. Thus two helpers
for the same descriptor are genuinely idempotent: a delayed helper which sees
`NONE(D)` cannot recreate `PENDING(D)`, and a delayed helper which meets a newer
generation cannot modify it. An older scanner's exact
`PENDING(D) -> NONE(D)` CAS necessarily loses after a later descriptor targets
`D+1` or beyond.

Every helper must also release-publish the global requested generation at
least `D` and the worker wake before it exact-clears `ACTIVE(table, D)`, even
when transfer observes `PENDING(D)` or `NONE(D)`. The latter can mean another
scanner completed quickly while an earlier helper was paused before publishing
the acknowledgement input. Repeating the monotonic requested-generation
publication is idempotent and preserves descriptor/token locator overlap.
An observer using descriptor absence as close evidence must acquire-observe
`IDLE` before it samples the requested generation, or recheck the requested
generation afterward. A full-pass acknowledgement must recheck that requested
maximum before publishing completion; it must never blindly clear a hint.

A transfer which sees token generation greater than `D` returns ordinary stale
`BUSY`, then rechecks the exact descriptor. If `ACTIVE(table, D)` still exists,
the newer token is impossible corruption and selects sticky no-reclaim; if the
descriptor changed, the helper is simply late and must not poison the newer
authority.

`INVALID` token state and malformed descriptor authority still select sticky
pin/no-reclaim containment. Observing a valid newer token generation is
ordinary stale-helper contention rather than corruption. Descriptor
generations are capped at the token encoding's 62-bit maximum. The maximum
generation itself may complete to `NONE(max)`; the next descriptor publication
pins instead of wrapping.

The older locally incrementing token helpers remain temporarily available for
already-landed dormant allocator/reclamation fixtures. Production table-rescan
handoff must use only the exact-target transfer/completion API. They can be
removed with the dormant-fixture migration after the live cutover.

## Required evidence before production cutover

- Pause helper B after exact descriptor recheck, let helper A transfer and a
  scanner complete, then resume B; the token must remain `NONE(D)`.
- Refresh `PENDING(D)` to `PENDING(D+1)` and prove a scanner holding `D`
  cannot clear it.
- Complete `D+1`, then deliver stale transfers for both `D` and `D+1`; neither
  may recreate work.
- Run two helpers and two scanners concurrently; transfer is idempotent and
  exactly one scanner clears each target generation.
- Compose descriptor reuse at the same address with a stale helper and stale
  scanner, and cover two tables whose per-table token generations skip global
  descriptor generations.
- Exercise the 62-bit maximum, invalid state pinning, same-address reuse, and a
  newer-generation stale helper.
- Race requested-generation acknowledgement against a newer descriptor and
  pause a scanner while physical reuse/unmap is attempted.

The dormant primitive checkpoint already covers delayed and simultaneous
helpers, simultaneous exact scanners, same-generation idempotence, stale
scanner/refresh, newer-generation stale helpers, `ACTIVE(0)`, invalid tickets,
malformed token containment, and the exact maximum. The strict standalone
model, 200 repeated runs, Clang ASan/UBSan, the focused M3 model gate, and the
complete M2 arena suite pass. The composed descriptor/enumerator/lifetime and
acknowledgement schedules above remain acceptance tests for the live cutover,
not claims made by this primitive-only tranche.

Bounded small/huge token enumeration, descriptor-to-token production handoff,
scan-proof completion, and close-predicate migration remain later coherent
tranches. In particular, none of this makes the dormant descriptor a live
reclaim grant by itself.
