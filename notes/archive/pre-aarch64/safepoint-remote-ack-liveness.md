Safepoint remote-ack liveness fix
=================================

Concurrent active `collectgarbage("collect")` calls exposed a safepoint
leader cycle: a thread waiting to become handshake leader called
`safepoint_ack_native()`, found a TG with `poll != 0` and `reqmask == 0`, and
then waited indefinitely for the consumed poll to clear. If the current leader
was waiting for pending acknowledgements, both sides could stall until the Lua
thread join timeout.

The consumed-poll wait is still required for mutator-side acknowledgement,
because that path is about to resume the TG's VM and must not race stack
scanning. Synthetic leader/native acknowledgements do not resume the remote TG,
so they now skip already-consumed polls instead of blocking behind them.

Regression coverage:
- `tools/ci/lua_test.sh m5_gc_total_atomic` now repeats the worker-side active
  full-collection smoke four times inside the Lua process.
- The same case passed 10 consecutive standalone runs after the fix.
