# Pinned generic-FFI post-call exit handoff (2026-07-18)

## Status and boundary

This note records the next activation boundary after the XSAVE-consuming
native-frame owner helpers.  It is not active yet.  The generic `IR_CALLXS`
recorder blacklist, the `jit_base` trace-flush remote-acknowledgement veto, and
trace-quiescence waits remain mandatory until every transition below has a
deterministic test.

The design is declaration-independent.  It adds no C-signature matcher and is
the common lifetime path for every scalar ABI shape which the existing x64
`CALLXS` classifier can lower.

## Existing proof reused

An active `LJFFINativeFrame` owns an exact `GCtrace *`, its original public
trace number, materialized stack offsets, and one native trace pin.  The pin
keeps the compact trace body, mcode areas, and public slot reservation resident
even after retirement clears `T->traceno`.  On x64, `lj_trace_exit()` already
accepts that retired reservation through `trace_exit_body_match()` and passes
the exact resolved body to `lj_snap_restore_exit()` while holding a GC2 SMR
reader.

That is sufficient for snapshot restoration only if native leave does not drop
the frame's last pin before the generated caller-state exit guard runs.

## Required owner state machine

The frame needs a stable `POSTCALL` state in addition to `ACTIVE`.  A frame is
well formed when it is synchronized and exactly one of these states is set:

- `ACTIVE`: the owner is inside the foreign call or still completing its
  consumed-poll leave boundary;
- `POSTCALL`: native state and callback mirrors are restored, but the exact
  trace pin has been transferred to the immediately following trace exit.

Ordinary return uses this order:

1. save the foreign errno/LastError pair;
2. call `lj_native_leave(L)` while the even `ACTIVE` frame and pin remain
   unchanged;
3. after its consumed-poll wait, restore surrounding callback mirrors;
4. publish the frame-stack sequence odd, clear/pop the frame, then publish the
   next even generation;
5. release the exact pin only after no stable frame names it;
6. restore the foreign error pair and perform fresh STOPREQ handling.

A forced return instead uses this order:

1. complete the same native-leave/poll and mirror restoration;
2. publish the frame-stack sequence odd;
3. change the top frame from `ACTIVE` to `POSTCALL`, retaining its exact trace,
   trace number, carrier and pin;
4. publish the next even generation;
5. restore the foreign error pair and return a nonzero post-call-exit value;
6. the unconditional caller-state guard exits the trace without replaying the
   foreign side effect.

The generated path may not execute any fall-through or linked tail while a
`POSTCALL` frame exists.  `RecordFFData.postcall_exit` already places its guard
after `lj_record_ret()`, in caller state, which is the required non-replaying
snapshot.

## Force decision and race closure

Entry records the owning TG's acknowledged handshake epoch.  Leave forces the
handoff when any of these is true:

- the returned safepoint action mask is nonzero;
- the TG acknowledgement epoch differs from the entry epoch, including a
  request consumed remotely while the foreign function slept;
- the callback slot changed from the entry sentinel;
- a future callback-suspension flag or trace-retirement request is present.

The owner must publish the frame sequence odd before its final epoch/flag
decision.  Otherwise a leader could admit the still-even pinned frame, publish
the force epoch, and race a leave path which had already decided to pop and
unpin.  An odd owner transition is never positive remote authority: readers
retry and reclamation stays closed.

## Exit cleanup

`lj_trace_exit()` resolves and leases the exact exited body before snapshot
restore.  After `lj_vm_cpcall(..., trace_exit_cp)` returns, and before either
the normal or negative-error return, it must attempt a nonthrowing cleanup:

1. acquire-load the top generic native frame;
2. require a stable `POSTCALL` state, matching carrier, exact trace pointer and
   original public trace number;
3. pop/clear it under the frame sequence;
4. release its exact native pin once;
5. leave a stable empty/lower-depth frame stack before VM rethrow, TEXIT work,
   GC defer, or interpreter polling.

No slot rediscovery is allowed for the release: the frame's exact pointer is
authoritative.  A mismatch is internal lifetime corruption and must fail
closed rather than guess which trace to unpin.  Cleanup must preserve the
foreign errno/LastError pair; x64 trace exit already brackets its body with the
authoritative OS-error save/restore.

Fresh STOPREQ raised inside the leave helper is a separate unwind edge.  It
must pop/unpin directly before throwing, because no generated post-call guard
will execute.  Callback-blacklist or result-conversion errors need the same
exactly-once cleanup guarantee.

## Trace-flush relaxation gate

Native remote acknowledgement of `EXIT_TRACES` or `FLUSHJ` may be enabled only
when the top frame can be admitted without raw-pointer guessing and proves all
of the following in one same-even snapshot:

- synchronized `ACTIVE` state;
- carrier equals the TG's current Lua state;
- saved JIT-base offset resolves to the current published `jit_base`;
- public trace slot contains the exact frame body;
- the exact body has a nonzero native pin.

The consumed poll then keeps the owner inside `lj_native_leave()` while the
leader scans roots and retires trace state.  Retirement may mutate routing
metadata, but generated code is not running during that interval.  When the
owner resumes, its changed acknowledgement epoch forces the caller-state exit,
and the reserved retired slot supplies the snapshot body until cleanup drops
the pin.

Unknown JIT activity, an odd/malformed frame, a missing pin, callback
suspension without its own certificate, or any admission retry retains the
current veto and wait.  Exact-frame support is an additional positive proof,
never a reason to make an unclassified native region optimistic.

## Deterministic gates

Before lifting the recorder or flush gate, tests must cover:

- normal enter/leave pops and unpins before returning zero;
- remote consumed handshake changes the entry epoch and returns force-exit;
- full flush retires a pinned body while the callee is parked, then the
  post-call guard restores from that exact retired body and releases the pin;
- callback observation forces the same handoff without replaying the call;
- snapshot-restore failure, STOPREQ, callback-blacklist failure and result
  conversion failure all leave zero leaked `POSTCALL` frames/pins;
- nested generic calls clean up in LIFO order;
- a handshake racing after native close but before the odd leave transition
  either owns the force handoff or remains conservatively blocked by
  `jit_base`;
- Linux SysV, macOS SysV and Win64 preserve both the foreign result registers
  and errno/LastError across enter, leave, guard and exit cleanup.

No `plan/` file is changed by this refinement.
