2026-06-27

- Routed `lj_chan.c` blocking channel waits through a fresh STOPREQ helper.
  Channel waits now snapshot the pre-existing sticky shutdown flag before
  entering native futex wait, poll a pending STOPREQ on leave, and only throw
  when the wait observed a fresh request.
- Added M3 coverage for STOPREQ interrupting blocked `channel:recv()` and
  blocked `channel:send()`.
- Added sticky timeout regressions for `channel:recv(timeout)` and
  `channel:send(value, timeout)`, so a stale shutdown flag does not turn normal
  channel timeout semantics into an interruption.
- Helper comments document why channel waits must use the fresh STOPREQ helper.
  The old source guard rejecting raw checks is obsolete.
