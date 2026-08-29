# ARM64 trace-publication GC checkpoint — 2026-08-27

This tranche adds a GC-only checkpoint for the future sealed first-side
publication suffix. It does not connect that suffix in `lj_trace.c`, and it
does not open the production ARM64 side recorder.

## Contract

`lj_gc_pubtrace_checkpoint_nodrain(g, traceno, body)` has two safe outcomes:

1. The exact trace-slot body is appended to the current owner TG's active SSB.
   The helper does not rotate or drain the SSB. The body becomes a raw traversal
   request through the existing release slot/cursor protocol.
2. If the exact slot does not match or the active SSB has no immediately free
   slot, the helper release-publishes `gc2.recovery_failed = 1`. That existing
   sticky lane participates in MARK-to-WEAK, WEAK-to-SWEEP, sweep-bridge, and
   physical-reclaim predicates, so no semantic GC reclamation can cross it.

The second result is deliberately a veto, not an error which permits trace
publication rollback. Production never clears it. It is preferable to an
unbounded recovery publication attempt inside a sealed transaction.

The helper contains no source loop, allocation, SSB rotation, drain, SMR wait,
handshake, Lua callback, error throw, or ordinary recovery scan. The only wake
is the existing nonwaiting worker wake after publishing the sticky veto.

## Phase-race argument

The exact body is queued regardless of the sampled GC2 phase. An IDLE-to-MARK
transition preserves existing SSB/grey work and later flushes/consumes it. If
MARK has already started, a raw SSB slot remains an explicit traversal request
even when a concurrent root snapshot marked the trace body first. Thus no
sampled-IDLE optimization can lose the publication race.

If immediate queueing is impossible, `recovery_failed` prevents MARK closure.
An IDLE publication may still race a new MARK start, but that cycle cannot pass
MARK-to-WEAK while the sticky lane is set and therefore cannot reach semantic
reclamation.

The more general recovery publisher was intentionally not reused: its exact
arena/huge recovery protocols are lock-free but contain retry/relookup loops.
The regular barriers may use those loops; the sealed trace suffix may not.

## Validation

`tests/t-arm64-jit-trace-publication-barrier.c` checks:

- an exact slot/body mismatch changes no active cursor and installs the sticky
  veto without manufacturing recovery work;
- IDLE queue publication followed by MARK start with the replacement SSB node
  deliberately withheld, proving the exact active slot survives the transition
  before it is traversed;
- queue publication after MARK is already active;
- a full active SSB with its replacement node withheld, producing the sticky
  close veto without SSB rotation or a synthetic recovery item, followed by a
  same-state phase-transition control after test-only veto clearing; and
- the production side-recorder gate remains closed.

`tools/ci/arm64_jit_trace_publication_barrier_contract.sh` also performs a
source-level exclusion check for loops, allocation, drains, recovery scans,
blocking SMR, handshakes, callbacks, and errors across both the GC2 internals
and the public GC wrapper before running the native fixture twice under both
arm64 and arm64e+BTI. It then restores the ordinary thin arm64 build and proves
that the GC2-only test helpers are absent.

The withheld-node case is a deterministic transition-boundary proof, not a
concurrent scheduler stress test. The eventual exact first-child publication
suite must additionally interleave the sealed publisher with a real phase-start
owner while retaining the same slot/body and SMR certificates.

The future caller must retain the already-established TraceVec/SMR and exact
body authority through this checkpoint. It belongs after GC root-link and the
exact `LJ_TRACE_PENDING -> body` slot CAS, and before topology publication and
the final authenticated parent-exit CAS.
