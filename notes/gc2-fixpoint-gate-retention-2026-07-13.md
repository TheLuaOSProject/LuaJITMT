# GC2 fixpoint gate retention

## Status and scope

This note records a cooperative-MARK regression and its local fix. It is
independent of the rootless destructor/FNEW tranche: destructor sidecars do not
participate in the JIT phase-gate or MARK fixpoint protocol.

The regression came from the cooperative MARK change in `bc17bd6f` (`Cooperate
with JIT during GC2 mark`). The intended policy lets bounded worker quanta
reopen the native gate for another mutator/JIT turn. The non-worker-owned
leader fixpoint path reused that drain machinery, however, and accidentally
applied the same reopen policy inside its own proof.

## Failure mode

`m3_gc2_recovery` repeatedly failed in the clean-fixpoint helper: 64 calls to
`lj_gc2_fixpoint_round()` could not report a clean MARK fixpoint. Inspection of
the failing helper showed that the only persistent blocker was
`gc2.jit_phase_gate == 1`. By the second round, `marks_this_round` was zero,
recovery count/failure were zero, and thread/table pending counts were zero.

The sequence was:

1. the leader closed the MARK gate at fixpoint-round entry;
2. its non-worker-owned drain called the ordinary worker drain machinery;
3. that nested drain saw MARK resume authorized and no close intent, so it
   reopened the gate as it would after a background worker quantum;
4. the leader's final gate check rejected the round, reopened/reset the root
   snapshot, and retried the same losing sequence.

This was a deterministic livelock in proof composition for the
non-worker-owned fixpoint caller, not undiscovered marking work. A
fixpoint-owned nested drain discarded a closure fact that its caller needed to
retain through every nested drain, including the optional post-`FLUSH_SSB`
drain, the root snapshot, and the final zero-work predicate. The worker-owned
MARK completion path was unaffected.

## Fix and behavioral contract

The worker drain now carries a private `hold_mark_gate` policy bit through
`gc2_worker_drain_logical()` into `gc2_worker_drain_inner()`. Gate reopening at
the end of a MARK drain is allowed only when that bit is false.

Only `gc2_worker_drain_budget()`, the private drain used by the non-worker-owned
fixpoint path, passes true. Public `lj_gc2_worker_drain()` and the ordinary
collect/step logical-driver calls pass false. The result preserves both halves
of the contract:

- A leader-side fixpoint round retains its closed-gate observation across every
  nested drain (including the optional post-`FLUSH_SSB` drain), the root
  snapshot, and the final zero-work predicate.
- An ordinary/background worker quantum may still close and quiesce MARK, do
  bounded work, and grant the next bounded mutator/JIT turn when MARK resume is
  authorized and no close intent exists.
- A fixpoint-owned nested drain never converts the leader's inherited closure
  proof into an ordinary cooperative mutator turn.

The flag is private policy plumbing, not a new synchronization primitive. No
waiting or stronger serialization was added. A concurrent peer may still
reopen the gate; the existing final phase, gate, and active-JIT checks reject
that round and retry safely. Worker-owned `lj_gc2_mark_complete()` remains on
`gc2_mark_drain_owned_bounded()` and is unchanged. SWEEP gate reopening is also
unchanged.

## Regression coverage

`test_reservation_gap_blocks_mark_close()` in `tests/t-gc2-recovery.c` now
locks down both behaviors in one fixture:

1. begin MARK and observe the gate open;
2. request MARK exit and observe it closed;
3. run an ordinary `lj_gc2_worker_drain(..., 1)` and verify that it reopens the
   gate for cooperative progress;
4. run the non-owner clean-fixpoint loop and verify that it completes with the
   gate still closed.

Focused validation passed:

- `m3_gc2_recovery`, including the helper fixture, the
  `LUA_USE_ASSERT + LJ_GC2_PARANOIA=1` fixture, and default-build restore;
- `m6_jit_gc2_readiness`, including hard-check, cooperative SWEEP,
  cooperative MARK, and readiness trace probes; and
- `git diff --check`.

The broader current session has also passed `m3_gc2_worker_scheduler`. These
results cover both the repaired fixpoint and the cooperative bounded-MARK
behavior that the fix deliberately retains.

## Intermittent one-shot recovery fixture

The aggregate scaffold first timed out in its nested normal recovery case. An
isolated recovery retry passed normal and then timed out in the paranoia case.
One immediate standalone paranoia retry passed, but that did not establish a
wrapper-only anomaly: ten standalone runs of the exact paranoia binary
produced nine passes and one reproducible 100% CPU spin in
`recovery_wait_paused()` during `test_grey_growth_transaction()`.

This was a real test liveness race, not a collector hang. MARK begins with a
50-microsecond cooperative native/JIT scheduling lease. The fixture enabled a
pause and launched a single one-shot `lj_gc2_worker_drain()` thread, assuming
that thread would reach the requested `LJ_GC2_RECOVERY_TEST_SSB_COMMITTED`
pause. It could instead legally honor the initial open-gate lease, consume its
one quantum, and return. With no drain thread left to publish the pause flag,
the main test thread spun forever in `recovery_wait_paused()`.

The fixture now calls `lj_gc2_jit_mark_request_exit()` and asserts
`gc2_jit_phase_gate_acq(g) == 0` before spawning the one-shot drain. This makes
the pause-point precondition explicit: the worker enters GC work rather than
spending its only quantum on the cooperative MARK lease. It does not change
collector policy; ordinary workers may still reopen the gate afterward under
the bounded cooperative contract documented above.

Post-fix validation resolved the intermittent failure: a forced-clean
`m3_gc2_recovery` passed its normal, paranoia, and default-build restore cases.
The exact normal recovery binary then passed 25/25 standalone runs, and the
exact paranoia binary passed 25/25 standalone runs, with every run completing
inside its 60-second limit.
