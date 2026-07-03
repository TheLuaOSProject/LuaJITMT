# Safepoint trace quiescence leader self

Trace-flush handshakes use the final quiescence pass to keep peer TGs from
running with trace slots or mcode that the leader is about to unlink. A TG
leaving compiled code publishes `jit_base` until `vm_exit_interp` restores
interpreter state, so peer TGs with `jit_base` or positive trace `vmstate`
remain blockers.

The handshake leader itself is different. Trace-exit C code can start side
recording and request a full trace flush before the x64 VM exit path has cleared
that leader's own `jit_base`. Waiting for the current leader in the quiescence
pass is therefore a self-wait: no peer can clear that field, and the leader
already applied its own safepoint action synchronously before quiescence.

`safepoint_trace_tg_active()` skips only the current TG whose tid matches
`hs_leader`. Every other live TG with `jit_base` or positive trace `vmstate`
still blocks trace retirement. `m3_safepoint_handshake` covers this with a
synthetic leader-owned `jit_base` `EXIT_TRACES` handshake; the fixture verifies
that the consumed poll clears while the VM-owned `jit_base` remains for the VM
exit path to clear.
