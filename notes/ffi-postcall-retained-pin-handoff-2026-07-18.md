# Generic FFI POSTCALL retained-pin handoff (2026-07-18)

## Status and scope

This tranche implements the exact lifetime handoff required after a traced
foreign call has already performed a non-replayable side effect.  It remains
dormant: the declaration-independent generic `IR_CALLXS` recorder gate is
still closed, and remote `EXIT_TRACES`/`FLUSHJ` acknowledgement still retains
the conservative `jit_base` veto.

No signature enum, C-shape matcher, wrapper family, or platform-specific
argument dispatcher is added.  No `plan/` file is changed.

## Frame states

A structurally valid frame is synchronized and has exactly one lifetime state:

- `ACTIVE`: native state is open, or its consumed-poll leave is still
  completing.  This is the only state which can certify a parked foreign call.
- `POSTCALL`: native state is closed and callback mirrors are restored, but the
  exact trace pointer, original public slot and one native pin are retained for
  the unconditional caller-state trace exit.

`lj_ffi_native_frame_push()` accepts only `ACTIVE`.  `POSTCALL` can be created
only by the owner's in-place finish transaction.  The structural snapshot API
accepts both states so teardown and trace exit can inspect the lifetime, while
the exact GC2 native scanner explicitly rejects `POSTCALL`: it is lifetime
authority, never parked-stack scanning authority.

## Leave ordering

The frame and pin remain unchanged while `lj_native_leave()` completes its
consumed-poll boundary.  A nonthrowing fresh-STOPREQ predicate is then sampled.
The owner performs the final transition in this order:

1. validate the exact top `ACTIVE` frame;
2. publish the frame sequence odd;
3. execute a full sequentially consistent fence, providing the required
   store-before-load edge;
4. acquire-read the callback slot, acknowledged handshake epoch, poll and
   pending request mask;
5. restore the surrounding callback/function/STOPREQ mirrors while odd;
6. either clear/decrement the top frame or replace `ACTIVE` with `POSTCALL`;
7. publish the next even sequence;
8. on the clear path, release the exact trace pin only after no stable frame
   names it.

The forced path is selected for any nonzero returned action, callback
observation, changed acknowledgement epoch, or newly pending poll/request.  It
returns `LJ_FFI_NATIVE_LEAVE_FORCE_EXIT` and keeps the pin.  The test-only
finish hook proves that callback and epoch decisions are sampled while the
sequence is already odd.

A fresh STOPREQ never retains the frame.  It selects the clear/unpin path, then
calls the throwing STOPREQ handler only after a stable empty/lower-depth frame
has been published.  Thus an unwind which bypasses generated guards cannot
leak a pin or leave TG teardown permanently busy.

## Trace-exit cleanup

`lj_trace_exit()` invokes `lj_ffi_native_trace_exit_cleanup()` immediately
after `lj_vm_cpcall(..., trace_exit_cp)` returns and before releasing its GC2
SMR reader.  This convergence point covers successful snapshot restore and the
protected error return before TEXIT events, GC work, side-trace recording or
VM rethrow.

Cleanup uses the current executing TG (`G2TG`), rather than the migratable
`L->tg_hint`.  Absence of a `POSTCALL` frame is the ordinary no-op.  A present
frame must match the current carrier, exact exited body pointer and original
public trace number.  Mismatch is internal lifetime corruption and fail-stops.
The frame is popped to a stable even generation before its exact pointer is
unpinned.  A duplicate cleanup therefore sees no frame and cannot double
release.  errno/LastError are preserved by both leave and cleanup.

The trace-exit SMR reader supplies a final independent lifetime proof while
cleanup inspects and unpins a body which may already be retired with
`T->traceno == 0`; the frame's original reserved slot remains the identity
check.

## Deterministic evidence

The x64 XSAVE fixture now proves:

- ordinary return still produces an empty frame stack and zero leaked pins;
- callback force publishes a coherent `POSTCALL` frame with `in_native == 0`;
- the exact scanner refuses that frame as a parked-stack certificate;
- direct exit cleanup pops once, unpins once and preserves errno/LastError;
- duplicate cleanup is a no-op;
- an acknowledgement-epoch change forces the same handoff;
- both callback and epoch sampling occur only while the sequence is odd.

The existing native-frame, generic-FFI and safepoint suites remain gates.  The
recorder gate means no production Lua/FFI call can enter this new path yet.

## Boundaries deliberately still closed

Remote trace-flush admission is not relaxed by this change.  Before doing so,
all of these coupled conditions must be implemented together:

- the consumed-poll wait may bypass trace actions for an ordinary trace-exit C
  frame, but must not bypass a certified `ACTIVE` foreign frame;
- trace-quiescence checks must recognize only a current-epoch, poll-held,
  same-even exact `ACTIVE` frame with a nonzero matching pin, or the leader and
  parked owner can deadlock on `jit_base`;
- a failed exact native scan must not count as completed remote
  acknowledgement;
- the post-call guard must be the first non-replaying exit after leave, before
  any result boxing/conversion guard which could restore an older snapshot and
  replay the foreign call;
- callback execution needs a distinct `SUSPENDED` state; a lower suspended
  frame must never satisfy `ACTIVE` scanner admission.

The next activation tranche must add authentic generated enter/call/leave/
guard lowering and prove success plus injected snapshot-restore error cleanup
before removing the generic recorder blacklist.
