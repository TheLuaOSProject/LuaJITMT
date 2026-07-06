# GC2 worker weak-drain fixture boundary

`test_async_weak` in `t-gc2-worker-scheduler.c` is a whitebox scheduler test for
parked-worker weak snapshot draining. It deliberately leaves the weak value on
the C test's Lua stack while checking that the weak-value entry is cleared.

That is not a full GC root-close scenario: a real weak-close root scan would
mark the stack-held value and the weak-value entry would survive. The production
runtime is correct to gate `lj_gc2_weak_drain()` on `weak_mark_closed`; clearing
before the mutable-root/SSB/grey frontier is closed would use an incomplete
liveness oracle.

The fixture now publishes `weak_mark_closed` itself before waking a worker. This
states the intended precondition explicitly and isolates the behavior under test:
parked workers can asynchronously clear an already-closed weak snapshot and
advance `worker_weak_drained`/`weak_clear_tables` telemetry.

Focused verification:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `src/luajit tools/test.lua m3_gc2_worker_scheduler`

Broader follow-up:

- `src/luajit tools/test.lua m3_gc2_scaffold m8_weak` now passes through
  `t-gc2-traverse`, `m3_gc2_worker_scheduler`, active thread roots, and the C
  safepoint handshake before stopping later in `m3_vm_safepoint` at
  `tests/t-vm-safepoint.c:637` (`g->gc2.hs_epoch == epoch0`).
