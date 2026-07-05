# GC2 Active Automatic Step Quantum

Automatic allocation-triggered GC steps now use two republish quanta:

- `LJ_GC2_HELPER_IDLE_STEP` while GC2 is idle, so allocation bursts still start a
  cycle promptly.
- `LJ_GC2_ACTIVE_AUTO_STEP` once GC2 is active, so allocation helpers remain a
  bounded progress hook instead of retrying mark-fixpoint root snapshots at the
  idle trigger cadence.

This is separate from public `collectgarbage("step")`, which keeps the explicit
stock-style step path. The active automatic path clears classic debt after one
bounded state-machine step and republishes the threshold farther out.

The focused closure allocation guard now passes against stock LuaJIT for the
default `closures_upval` row on Linux/x64 (`1.428933x` in the local run). A
one-million-closure probe no longer retried major root snapshots during the hot
allocation loop; the remaining activity was pending-root flush and bounded grey
worker assist progress.
