# Safepoint Poll Wake Invariant

`safepoint_wait_consumed_ack()` no longer uses a periodic 1 ms wake while a
thread is waiting behind a consumed native ack.  The wait is now valid because
late request publication wakes the same poll futex after storing `reqmask`.

The ordering is:

1. Publish the action mask to `TGState.reqmask`.
2. Publish `TGState.poll = 1`.
3. Wake `TGState.poll` waiters.

That wake matters for the race where a thread observes `poll != 0` and
`reqmask == 0` while a consumed native ack is still being completed.  A later
handshake request can now make `reqmask` visible and wake the waiter directly,
so the waiter does not need a timed fallback to discover new work.

The unit coverage is `test_consumed_ack_reqmask_wake()` in
`tests/t-safepoint-handshake.c`.
