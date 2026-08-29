# GC2 recovery cycles and b1.2 minor fallback (2026-07-13)

This note records a release-critical recovery correction and one deliberately
temporary generational-GC boundary. The plan files remain unchanged.

## Recovery cycle correction

During SWEEP, an admitted recovery traversal used the same public semantic root
path for its private child edges. A cyclic container could therefore publish
its own already-CLAIMED recovery identity, change it to REDIRTY, replay, and do
the same thing forever. The observed table-resize stress stall was this loop,
not missing recovery work: every locator reconciled, while the redirty count
grew without bound.

The correction separates two meanings:

- public roots and mutation barriers remain semantic publications and always
  retain a racing replay, including for an already-current table;
- child edges discovered by an admitted worker traversal are private graph
  discovery and may consume an already-covered same-cycle descendant.

Tables use their completed scan-cycle/dirty-epoch proof. Other graph-bearing
containers use `LJ_GC_NEEDSCAN` only after an exact current-major mark/scheduling
event. `gc2_markobj_preserve_status_impl()` clears an uncounted token on the
unique NEW transition; other NEW winners already leave explicit queue work
which must drain before phase close. A two-cycle unlinked-closure fixture proves
that bounded root cleanup is not the correctness mechanism.

Weak-mode metadata needed the same distinction. Worker traversal now retains
the metatable and `__mode` string bodies through expected-type scopes without
treating that metadata read as a new SWEEP root. Public weak-write/barrier
callers retain semantic admission. A full-SSB `setmetatable(t, t)` fixture
proves the self-metatable recovery identity completes without REDIRTY.

Recovery accounting was tightened at the same boundary:

- aggregate and huge-lane rollback refuse zero with CAS and fail-stop in every
  build instead of wrapping in release mode;
- the exact huge-lane subcount is included in the public stats snapshot;
- terminal discard first counts all authoritative main/small/huge locators
  without changing them and reconciles both counters before clearing or
  unmapping anything;
- after the joined-world destructive pass it rechecks the proven totals, clears
  the huge subcount first, and clears the aggregate veto last.

## Accepted b1.2.0 generational fallback

The non-table token proof is exact for major cycles. It is not yet proven for a
minor that deliberately retains old marks: some g-only IDLE non-table mutation
paths still need a complete parent-qualified remembered-barrier audit.

For b1.2.0, `gc2_update_public_minor_gates()` therefore keeps both
`minor_sweep_enabled` and `minor_roots_enabled` at zero. Fork-local
`threading.gcmode("generational")` remains accepted and remembered publication
remains active, but requested minor work falls back to a full major mark/root
and sweep cycle. Telemetry continues to report the request and deferral. This is
a correctness-first fallback, not a claim that generational GC is complete.

Re-enabling physical minor sweep/root elision is b1.2.1 work and requires:

1. an audit that every mutable non-table old-to-new edge has an IDLE
   parent-qualified remembered publication;
2. focused remembered-mutation tests for FUNC, UPVAL, UDATA, PROTO, and TRACE;
3. major-to-minor stale-token and abort-carried-queue tests;
4. paranoia, M9/M10, sanitizer, and performance validation with both public
   minor gates enabled.

## Focused evidence

The recovery fixture covers full-SSB self-array and self-metatable tables,
cyclic C closures and channel userdata, public-root CLAIMED-to-REDIRTY replay,
two-cycle stale-token clearing, rollback underflow fail-stop, huge completion
underflow, and non-destructive terminal mismatch preflight. Normal and paranoia
`m3_gc2_recovery` pass. The release table-resize stress completes in about ten
seconds under its 30-second gate rather than entering the prior multi-minute
REDIRTY loop.

The independent M9 unique-key cadence gate remains a release-performance
blocker outside this recovery patch: untouched `ae393275` reports
`worker_delta=283`, `allocated=9,582,476`, while this revision reports the same
`worker_delta=283`, `allocated=9,582,572`; both exceed the existing `<=160`
limit. The limit was intentionally not raised. The stock comparison also still
reports the known `tab_insert_newkey` regression (about 12.1x versus stock).
Those results demonstrate parity with the frozen base, not release acceptance;
the cooperative JIT/performance tranche must close them before b1.2.0.
