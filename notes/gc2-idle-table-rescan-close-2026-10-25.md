# GC2 idle table rescan versus sweep close (2026-10-25)

The threaded ARM64 tmpbuf fixture intermittently reached the existing
`table NEEDSCAN leaked to normal sweep close` assertion after
`lj_gc2_sweep_to_idle()` had changed the phase to IDLE, released the worker
token, and run its idle-transition grace handshake.

An assertion-enabled native diagnostic build reproduced the close with
`table_rescan_pending == 2`. A small temporary ring around the real reservation
helper showed the remaining counted table tokens were reserved with
`gc2.phase == LJ_GC2_IDLE` in cycle 56. The handoff came from normal table
stores which resumed during the IDLE grace handshake.

Ordinary IDLE table stores continue to reserve a counted token for the upcoming MARK
cycle. Only the short terminal SWEEP->IDLE close grace uses uncounted SSB
publication. A dedicated sequentially-consistent close flag is set after the
close owner acquires the phase gate and before either pending-count predicate;
it remains set through the post-IDLE assertions and is cleared before the
phase gate is released. It is distinct from generic GCSCAN, which also protects
MARK->WEAK and WEAK->SWEEP and therefore cannot authorize dropping a current
cycle reservation.

Reservation increments and close pending reads participate in one SC order
with the dedicated flag store/load. Either a publisher increments first and
the closer sees the outstanding reservation and retries, or close flag
publication wins and the late publisher emits a concrete SSB locator without
creating a counted token. If a publisher had already exposed INSTALLING, its
unique installer settles only its own reservation after observing the flag.
The final idle handshake waits for participant acknowledgments, so a mutator
which crossed the initial flag check must settle its provisional reservation
before the close checks zero pending after that handshake. The flag remains set
through all close cleanup. Ordinary IDLE stores remain counted, so a store
overlapping IDLE->MARK startup
cannot lose current-cycle marking behind an uncounted SSB push.

The table-store guard regression checks ordinary IDLE stores through a full
collection, including suspended-coroutine and second-universe stores. It also
preserves the successful active-SWEEP finish test, verifies generic GCSCAN
still retains COUNTED work, pauses a real publisher at INSTALLING while a
concurrent observer sets the close flag, then checks that only its own
reservation is settled and the SSB remains nonempty. A second close model
checks the direct late-store path and verifies that publication leaves no
counted reservation before close publishes IDLE.

Validation on a disposable native ARM64 bootstrap/assertion build:

- `t-gc2-table-store-guard`: passed in the disposable native ARM64
  bootstrap/assertion build.
- `tests/t-arm64-tmpbuf-thread.lua`: passed after the close-only flag
  refinement, then passed three consecutive runs after the final flag placement.
- Negative control: compiled the unchanged `HEAD:src/lj_gc2.c` against the
  final header/layout and regression fixture in
  `/private/tmp/gc2-rescan-negative2`. The ordinary IDLE and generic-GCSCAN
  assertions and the INSTALLING observer setup passed; the close-window
  assertion failed because the old code left a counted reservation
  (`pending_before` changed), as expected.
- ARM64 object inspection after the rebuild showed inline atomic operations;
  `llvm-nm -u src/lj_gc2.o` showed no `__atomic` or `__sync` helper dependency.
- Diagnostic baseline: reproduced the close assertion and identified the
  phase-IDLE reservation source. Diagnostic instrumentation was confined to a
  disposable `/private/tmp` source copy.

This evidence covers the targeted publication/close race and focused runtime
fixtures. It does not replace the full ARM64 bootstrap gate.
