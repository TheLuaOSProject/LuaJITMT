# GC2 worker TG handshake identity

This slice gives parked GC2 workers real TG/safepoint identity without letting
them publish `P_IDLE` from the worker loop yet.

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

Worker-owned `P_SWEEP -> P_IDLE` publication remains deliberately deferred.
The current legacy `lua_gc()` driver can run inside non-pollable C GC code; a
worker-initiated close handshake can wait on that mutator while the mutator is
not at a VM poll or native boundary. The worker still drains mark, weak, and
traversable sweep work, but the legacy/mutator close path remains responsible
for publishing idle until the GC scheduler no longer depends on that legacy
driver boundary.
