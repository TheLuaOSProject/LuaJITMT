State owner wait primitive
==========================

- `lua_State.thr_owner` is now a wakeable wait word for internal ownership
  handoff paths. `lj_state_release()` wakes waiters after publishing owner 0.
- `threading.join()` result claiming and TLS-less foreign callback attach no
  longer use blind 1ms sleeps while waiting for a child/carrier state owner to
  change. They wait on the target state's owner word when it is nonzero and
  fall back to the shared retry-yield helper for transient ownerless races.
- This preserves `lj_state_claim()` as a nonblocking probe for general callers;
  blocking behavior is explicit through `lj_state_owner_wait()`.
