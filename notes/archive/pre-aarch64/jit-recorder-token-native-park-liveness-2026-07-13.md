## Recorder-token ownership at blocking native parks

The b1.2.0 heavy threaded-flush gate exposed a recorder-token liveness cycle.
A main TG entered `threading.thread:join()` while it still owned the global JIT
recorder token. A child running `jit.flush()` asynchronously changed the
recorder state from active `LJ_TRACE_RECORD` to aborted `LJ_TRACE_RECORD`, then
waited to acquire that token. The owner was already parked waiting for the
child, and only the owner could finish recorder cleanup and release the token:

```
main:  thread:join -> native futex park (owns jit_token)
child: jit.flush   -> lj_jit_token_acquire_wait (must finish before exit)
```

This was not a slow stress case. The exact heavy case failed twice at its
60-second join timeout; GDB showed `jit_token == main_tid`, `J->L == main`,
`J->state == LJ_TRACE_RECORD` with `LJ_TRACE_ACTIVE` cleared, the main TG in
`threading_futex_wait_l()`, and the sole live child spinning in
`lj_jit_token_acquire_wait()`.

### Rule

A Lua/TG owner must not enter a potentially blocking native park while it owns
unpublished recorder state. Immediately before the actual park it calls
`lj_trace_abort_owner()`, which discards that owner's unpublished trace state
and releases the token. Published traces are unaffected.

The threading library applies the rule in its common futex-wait wrapper, which
covers contended join, spawn/activation, mutex, and lifecycle waits.

The rule is kept at concrete blocking sites instead of in
`lj_native_enter_l()`. That helper publishes a native stack snapshot and is not
itself a promise that its caller will block; making every future native-entry
caller abort would unnecessarily constrain nonblocking C paths.

### Channel audit and reentrant recorder boundary

Channels have a separate futex substrate in `lj_chan.c`, so ordinary blocking
and timed channel waits apply the same pre-park owner teardown after their
optimistic spin fails. Try operations, zero-timeout operations, and handoffs
completed during the spin remain untouched.

A direct, unguarded application of `lj_trace_abort_owner()` was tested and
rejected: a trace `start` event callback can block while `trace_state()` is
still on the C stack. Tearing down `J->cur` reentrantly from that callback
caused an immediate production-build segmentation fault. This applies equally
to channel and threading-library parks.

Each concrete park calls `lj_trace_abort_owner_before_park()`, which compares
the current TG id with `vmevent_owner_acq(g)`. The exact VM-event callback owner
keeps the token and lets the outer recorder frame perform cleanup after it
unwinds; all ordinary parks abort and release before sleeping. The guard is
deliberately not folded into `lj_trace_abort_owner()`, whose detach/teardown
callers have different lifetime obligations.

The safe follow-up boundary is one of:

- make contended JIT control logically invalidate/queue work without waiting
  for a token held by the callback; or
- publish an explicit recorder-callback/reentrancy state, request abort, avoid
  the park, and finish cleanup only after `trace_state()` unwinds.

The guarded callback still cannot synchronously wait for a peer whose
`jit.flush()` must acquire its token. A bounded channel receive or timed join
returns first, after which recorder unwind lets the peer finish. Making that
cross-TG dependency synchronous requires the broader nonblocking JIT-control
protocol above and remains b1.2.1 debt.

### Validation

- The pre-fix heavy case reproduced the join timeout twice.
- The fixed production build completed the exact heavy threaded-flush case in
  five runs, including three consecutive repetitions.
- The normal threaded-flush, safepoint-handshake, and recorder-token gates were
  rerun after the fix.
- A deterministic join reducer keeps one single-round background worker and
  repeats the existing hot churn/flush loop. The unfixed exact source failed at
  round 40 in three consecutive runs; the fixed build completed all 96 joins.
- A production Lua regression enters both a timed channel receive and a timed
  join from a trace `start` callback. Both waits return their bounded timeout,
  preserve the active recorder frame, and allow the peer flusher to complete
  after callback unwind; this catches the unguarded teardown crash.
- The focused join and VM-event park regressions also pass a clean
  `-DLUA_USE_ASSERT` build.
