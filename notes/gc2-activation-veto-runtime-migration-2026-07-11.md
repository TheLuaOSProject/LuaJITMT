# GC2 activation veto-only runtime migration

Status: staged runtime safety overlay. `LJGC2Activation` is now embedded in
`GC2State`, but it is deliberately **not** a reclamation authority.

## Published behavior

The token starts as `IDLE/ROOT_OPEN`, mark epoch zero, generation zero. The
seven legacy phase writes are dual-mirrored:

| Legacy write | Typed migration edge | Ordering |
| --- | --- | --- |
| unpublished initialization | `IDLE/OPEN` | before global publication |
| `IDLE -> MARK` | `IDLE -> MARK` and next synthetic epoch | typed first |
| `MARK -> WEAK` | `MARK -> WEAK` | exact legacy CAS first |
| `WEAK -> SWEEP` | `WEAK -> SWEEP_OPEN` | typed first |
| preserve abort to IDLE | compatible active state to `IDLE` | typed reset under exact phase gate |
| normal sweep close to IDLE | abandon `SWEEP_OPEN/OPEN` to `IDLE/OPEN` | mirror the xchg-returned source |
| forced cycle close to IDLE | compatible active state to `IDLE` | worker + phase gate, then exact reset |

The temporary mark epoch advances once per legacy cycle. It is only an ABA
diagnostic until epoch markwords become authoritative; minor-cycle epoch reuse
is not enabled by this migration.

MARK staging occurs before consuming force-major state, changing cycle
counters, clearing queue-deduplication bits, or resetting mark state.  The
initiating request tid remains in `cycle_leader` through legacy MARK
publication and the complete mark/barrier/root-handshake initialization.  The
last initializer exact-CASes only its captured tid to zero.  A failed admission
recheck exact-rolls back its staged typed MARK without touching a replacement
request.

Every invalid exact source, invalid transition, or generation saturation is
converted to sticky `NO_RECLAIM`. A reclaim reader which merely observes the
brief mismatch between the two separately published words returns a temporary
veto and does not pin: only the phase actor which owns the exact mirror edge may
decide that the mismatch is real.

An IDLE xchg observer which returned IDLE did not own an edge and therefore
does not adjudicate a still-active typed source. An actor which did change the
legacy phase must prove a compatible typed source; active legacy beside typed
IDLE is a missing mirror and becomes sticky `NO_RECLAIM`. The defensive reset
accepts legacy WEAK with typed MARK or SWEEP_OPEN, preserving fail-closed
behavior for binaries/tests paused at either historical dual-publication
boundary. Exact phase-gate ownership makes those states defensive recovery,
not an expected steady-state runtime schedule.

## Exact phase and worker exclusion

`cycle_leader` now doubles as a nonwaiting phase-edge gate. MARK-to-WEAK,
WEAK-to-SWEEP, preserve/forced close, and normal sweep close claim only the
exact `0 -> LJ_THREAD_GCSCAN` CAS and release only the exact inverse CAS. They
never overwrite a queued cycle request or clear a replacement owner.

Forward actors hold GCSCAN through every post-CAS phase initialization,
including SWEEP cursors, string-sweep publication, barrier handshakes, and the
last boundary drain. Preserve-root publication which cannot abort immediately
rescues its exact object into MARK/WEAK/SWEEP; it never relies on the
IDLE-generational remembered set after losing the gate.

The worker token and GCSCAN use try-only mutual exclusion:

- worker claims reject GCSCAN before and after their CAS;
- opportunistic preserve abort proves `worker_active == 0` before and after
  claiming GCSCAN;
- forced close and normal sweep close claim the worker token first, then
  GCSCAN, and revalidate the SWEEP bridge under both.

No actor waits for either token. A loser preserves/requeues concrete work and
returns. This prevents a paused MARK initializer, weak/sweep transition, or
sweep-bridge worker from resuming state mutation after another actor publishes
IDLE.

## Temporary staged-authority abandon edge

The general transition graph still forbids `SWEEP_OPEN -> IDLE`, because an
authoritative collector must pass through `SWEEP_CLOSING` and `SWEEP_COMMIT`.
The migration adds one narrowly named exact operation which accepts only:

```text
SWEEP_OPEN / ROOT_OPEN -> IDLE / ROOT_OPEN
```

It means “discard this veto-only staged authority.” It does not mean that root
admission closed, that descriptors were covered, or that reclamation committed.
Both pre-bridge abort and normal legacy close use it only after the legacy
protocol has made its own decision. Delete this edge when the typed gate becomes
the close/reclaim authority.

## Reclamation vetoes

Activation validation is an additional negative prerequisite at:

- small traversable-arena physical sweep;
- huge mapping physical sweep/unmap;
- the central sweep SMR entry (covering sweep-side retired JIT state); and
- the ordinary `lj_gc2_reclaim_retired()` drain.

A coherent `IDLE` or `SWEEP_OPEN` snapshot only means “the migration overlay has
not vetoed this operation.” Existing phase, worker-token, grace, SMR-reader,
arena-gate, and retire-epoch checks remain the sole positive permission. TG body
reclamation is intentionally not covered by this checkpoint.

## Remaining limitations

- No runtime root writer publishes an exact root descriptor yet.
- The root gate remains `OPEN`; runtime code never publishes `CLOSING` or
  `COMMIT`.
- The token cannot prove a root snapshot, mark fixpoint, grace, or destructor
  right.
- Physical reclaim sites not routed through the listed small/huge/retired paths
  still rely solely on their legacy protocols.
- TG reclaim is excluded until stable TG handles and leases replace raw-body
  lifetime assumptions.
- Active-phase `preserve_root` rescue is valid for its current callers, which
  publish a persistent native/root-list reference before invoking it. It is not
  a generic admission API for ephemeral unregistered roots; those require an
  exact root descriptor or a retrying admission protocol.
- The two-word mirror intentionally has short mismatched intervals. They are
  fail-closed for reclaim but are not an atomic replacement for the legacy
  phase word.
- `cycle_leader`/worker exclusion is a migration ordering mechanism, not proof
  that root admission or reclamation committed.

Focused tests cover primitive abandon validation and saturation, complete
runtime phase cycles, retained MARK-request ownership, exact sentinel
non-overwrite, MARK and pre-bridge SWEEP worker exclusion, defensive
dual-publication skews, active/typed-IDLE missing-mirror pinning, staged MARK
rollback without replacement-owner loss, completed/in-flight forward-abort
deferral, central sweep-SMR admission, and preservation of a small arena, huge
mapping, and reclaimable retired table vector under mismatch and sticky vetoes.
