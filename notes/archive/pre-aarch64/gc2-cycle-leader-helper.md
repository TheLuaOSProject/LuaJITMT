# GC2 cycle-leader helper slice

This slice routes the GC2 nonblocking cycle-request token through helper
accessors:

- `gc2_cycle_leader_store_rlx()` initializes the token to idle.
- `gc2_cycle_leader_cas()` claims cycle leadership and releases only the exact
  value owned by that actor.
- MARK retains its nonzero request token through phase publication, mark/weak
  resets, and the barrier/root handshake. The last initializer exact-CASes its
  captured request to zero; it never clears a replacement value.
- `LJ_THREAD_GCSCAN` is an exact phase-edge gate. MARK-to-WEAK,
  WEAK-to-SWEEP, preserve/forced close, and normal sweep close claim only
  `0 -> GCSCAN` and release only `GCSCAN -> 0`.
- A requester rechecks legacy IDLE after its CAS and exact-releases a request
  which lost the phase race. This prevents a paused IDLE sampler from stranding
  a tid token after another actor has published MARK.
- Release stores and unconditional exchange are no longer production ownership
  operations; their accessors remain only for low-level fixtures/compatibility.

The phase gate and `worker_active` use try-only mutual exclusion. Worker claims
reject and recheck GCSCAN. Close actors either own the worker token or prove it
zero before and after claiming GCSCAN, so no paused worker can resume phase
initialization or sweep publication after IDLE. No path waits for either owner.

Production runtime users in `lj_gc2.c` no longer spell ad hoc atomics against
`GC2State.cycle_leader`. Production access to the token must stay behind the
documented helper surface; observable behavior is covered by the named fixtures.

Validation:

- `tools/ci/lua_test.sh m3_gc2_worker_scheduler`
- `tools/ci/lua_test.sh m6_jit_alloc_account`
- `tools/ci/lua_test.sh m10_generational`
- `git diff --check`
