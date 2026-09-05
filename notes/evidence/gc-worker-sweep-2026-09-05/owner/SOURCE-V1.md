# Candidate 1 source freeze, worker SWEEP bridge scheduling

Base: runtime commit 79345529bd932e68f8159ec17224467a10cad09b.
Shared docs HEAD at creation: e3b2ec6afc4f6819fad7fad84dc179c250196155.
The previous candidate3 diagnosis is unchanged and copied only under previous/.

Only src/lj_gc2.c changes. The existing public void wrapper and every caller
remain. Its internal helper owns the existing worker_active claim, preserves
all phase/recovery/finalizer/native/root/SSB/NEEDSCAN/READY gates, and decides
an explicit result before releasing that claim. No Lua state is acquired or
borrowed; the worker keeps its existing TLS TG. No new IDLE request or finalizer
callback is introduced. Existing calls may still wait synchronously for a
handshake or walk a whole detached pending chain at root EOF.

The result counts an actual unlink, normalized root cursor change, new root EOF,
new completed root snapshot, new READY latch, or all previously incomplete
allocator preparation reaching the cycle certificate. Comparing NULL as the
root-head slot avoids false progress on a first failed edge validation. A changed
deferred epoch wins over other progress. A same-claim phase/cycle recheck guards
all positive results. Graph-only helper work is conservatively not a bridge
progress result. A previously complete bridge returns COMPLETE.

The worker invokes the helper only for active SWEEP, after leaving its drain's
worker claim and after checking worker STOP. BLOCKED or DEFERRED reuses the
existing minimum backoff, consuming self-wakes until its deadline. A certified
advance explicitly continues to the next scheduling unit even if no producer
changed worker_wake. Stop is checked before that continue. Existing ordinary
IDLE-close and wake-before-wait logic remain.

Shutdown source assessment: gc2_worker_stop_locked_l first publishes worker STOP
and wakes the pool, then marks the joining Lua caller (or its exact physical TG
for the no-L API) native before pthread_join. RESET_ALLOC and SCAN_ROOTS use the
existing remote-native acknowledgement and retain poll through leader cleanup.
The post-join native leave returns/accumulates pending actions through the old
L-aware path. No new cancellation of an admitted handshake is proposed: a worker
already inside the synchronous boundary must finish that ownership protocol,
then observe STOP. A noncooperating external actor can still delay the handshake;
this is not a nonblocking shutdown proof. Exact pending-RESET/SCAN stop/join
runtime controls and broad validation are still pending at this source freeze.

This candidate excludes the independent constructor-owned quarantine defer
repair. Its original counterexample remains a separate expected limitation.
No runtime tests have been run for this candidate at this freeze.
