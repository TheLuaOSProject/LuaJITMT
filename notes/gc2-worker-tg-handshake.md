# GC2 worker TG handshake identity

This slice gives parked GC2 workers real TG/safepoint identity and lets them
publish `P_IDLE` from the worker loop once the legacy sweep boundary is ready.

Parked workers now allocate a dedicated `TGState`, set it in OS-thread TLS,
enter a no-Lua-stack native region, attach to the GC2 TG registry, and detach on
exit. The pthread handle and TG lifetime are split: the handle is freed after
join, while the worker TG is finalized only after `lj_tg_reclaim_dead()` has
unlinked it from the lockless TG list. If a dead worker TG is still registered
because another live TG prevents reclamation, a worker restart fails instead of
reusing the same list node.

Safepoint handshakes now have an `hs_leader` owner word. A would-be leader waits
for the current leader to finish and acknowledges any pending handshake while it
waits, avoiding a leader-wait versus ack-wait deadlock. Native TGs with no
`lua_State` can now be remote-acked, which is required for parked worker TGs.

The first worker-owned `P_SWEEP -> P_IDLE` attempt exposed a collect-time
deadlock because the legacy `lua_gc()` driver was running non-pollable C GC
steps. `gc_onestep()` now polls for pending safepoints before each state-machine
step, so a worker close handshake can be acknowledged while the mutator is
driving a synchronous full collection. This is still a bridge: legacy sweep
publishes the close-readiness latch and still owns the final Lua GC state
transition to `GCSpause`, but the parked worker may now publish the GC2 idle
phase after all sweep predicates are clear.

Stopping parked workers also marks the caller's TG native while joining worker
pthreads. Without that, `collectgarbage("workers", 0)` could wait in
`pthread_join()` while a worker was waiting for that same caller to acknowledge
an idle-close handshake.
