# Finalizer Root Scan Owner

## 2026-06-20

Bug:
- GC2 root scanning walked `GC2State.finalizer_mpsc` directly while the
  single-consumer finalizer drain could exchange that stack and reverse its
  `nextgc` links in place.
- A scanner that loaded the producer head before the drain completed could stop
  at a rewritten link and miss still-pending finalizer objects until the next
  cycle.

Fix:
- `gc2_scan_pending_roots()` now enters the GC2 finalizer owner, drains the
  producer stack into the stable `finalizer_tail` ring, scans that ring, and
  then leaves the owner.
- The GC2 paranoia checker uses the same owner/drain path before validating
  queued finalizers, so paranoia no longer races the MPSC drain either.
- This matches the legacy finalizer mark path and keeps the producer stack as a
  publication queue, not a concurrently traversed root list.

Regression:
- `tests/t-gc2-phase.c` now pauses a peer finalizer drain after the MPSC stack
  is exchanged but before the ring tail is published, starts a GC2 mark scan,
  releases the drain from another thread, and asserts the queued object is
  marked only through the finalizer queue path.
