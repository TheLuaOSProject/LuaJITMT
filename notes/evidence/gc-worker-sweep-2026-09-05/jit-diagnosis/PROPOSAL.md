# Foreground scheduling for cooperative JIT SWEEP without workers

This is a reviewable source proposal, not an implementation. The reproduced failure is a service-rate mismatch: the no-worker compiled mutator creates durable rescue work faster than its existing foreground schedule drains it. The SWEEP bridge has already completed. Existing safety gates correctly refuse physical reclamation while that graph remains open.

## Exact source path

The frozen baseline source is copied in `source/` with original line numbers.

1. `lj_asm_x86.h:4168` emits `asm_gc_check`: ordinary `gc.total >= gc.threshold` or the published hard cadence calls `lj_gc_step_jit`; `asm_xpoll` at 4206 also observes the JIT phase gate. Generated code therefore reaches the existing helper at ordinary pacing checks.
2. `lj_gc.c:4134` (`gc_jit_phase_threshold_exit_due`) explicitly excludes an open SWEEP from threshold exits. `lj_gc_step_jit` at 4385 instead advances `gc.threshold` by `LJ_GC2_ACTIVE_AUTO_STEP=4096` while leaving that trace running. With zero workers there is no background consumer; the worker wake branch at 4394 is inactive.
3. `lj_gc2.c:3245` (`lj_gc2_flush_alloc_checkpoint`) notices a real traced allocation past the hard cadence, increments `jit_hard_checks`, performs the existing SWEEP no-op assist, then calls `lj_gc2_jit_sweep_request_exit`. The latter closes native entry and publishes `jit_sweep_displaced=1`. `lj_gc2_hard_check_advance` moves the next check by `LJ_GC2_TRACE_HARD_CHECK_BATCH=524288` bytes. The observed adjacent hard events are 525824 bytes apart because the production accounting checkpoint batches debt.
4. `lj_trace.c:6849` recognizes the GC exit after snapshot/event/SMR work, publishes this TG's `jit_base=NULL`, then calls `lj_gc_step`. This handoff is functioning: all 50 observed exits reach `gc2_step_auto` with quiescent JIT state and a 64-step budget.
5. `lj_gc2.c:22211` prioritizes published SSB/grey work in the bounded SWEEP worker quantum. Recovery is reached only when no moved SSB entry/grey object remains. All 50 observed handoffs commit exactly 64 SSB entries, advance no recovery item and finish no arena.
6. At `lj_gc2.c:22582`, that quantum calls `gc2_jit_phase_gate_open_sweep`. The helper at 150 consumes `jit_sweep_displaced=1` and grants a fresh 50000ns native turn even though `mutator_turn` was passed as zero. The next inner step observes that new lease and returns without draining more graph work. The one-shot machine-code observation in `lease-decision.jsonl` proves an actual true refusal after the clock comparison, before debugger delay.
7. `gc2_step_auto` at `lj_gc.c:4159` can spend the remaining 63 attempts on that refusal, then advances the ordinary threshold at 4218. Subsequent open-SWEEP soft checks advance it again. Thus the next meaningful foreground graph service is normally the next 512KiB hard cadence, about 5056 new 104-byte tables in this fixture, against 64 SSB slots of service. The exact sampled interval is recorded in `hard-handoff-accounting.json`.
8. The unchanged 2ms native waits exceed the lease duration but do not themselves create a collector request. `threading_sleep` -> `lj_thr_sleep_ns` -> `lj_native_leave` -> `lj_safepoint_poll` performs the real native publication/poll protocol. With poll/reqmask/profile request zero, that poll returns without a GC driver. Automatic allocation subsequently resumes through the same compiled soft/hard geometry.

The late work is not stationary in every respect: 3200 SSB entries are consumed across those 50 handoffs. Recovery remains unserved and grows; arena completion stops. Calling this a total lack of collector work, a lock deadlock or a failed JIT exit would misstate the evidence.

## Smallest implementation boundary to review

Keep the repair inside no-worker automatic SWEEP scheduling, using the existing trace exit and ordinary interpreter driver. The first prototype should retain an unpaid foreground scheduling obligation when concrete rescue work remains, and should consume that obligation only after an appropriate bounded unit commits. An ordinary threshold update must not silently replace an unpaid obligation with another allowance to allocate.

Two closely related alternatives need review before choosing the exact representation:

| Alternative | Source scope and benefit | Remaining proof obligation |
| --- | --- | --- |
| Restore ordinary threshold-driven exits in no-worker SWEEP | In the C phase/threshold decision, use the ordinary threshold as an exit reason when `n_workers==0`; publish the existing SWEEP exit request so snapshot restoration sees a durable closed gate even if callbacks/accounting move the pacing number. Leave worker-backed wake behavior intact. This uses existing generated GC checks and the existing trace-exit root handoff. | A threshold exit alone can still meet an unexpired 50us lease. The next prototype must measure completed graph work per allocation interval and prove the original workload finishes; entry frequency alone is not a service guarantee. Review `asm_gc_check`, checkpoint debt flush and XPOLL together. |
| Carry bounded foreground work debt across native turns | Add a precise automatic-driver outcome or retained request that survives a lease-only refusal. Stop spending the remaining bounded batch on an unchanged lease; schedule the next normal safe-boundary unit while debt remains. Couple the granted native allocation turn to completed rescue work or use backpressure until the old graph is serviced. | The request/continuation must be durable, survive debt flush/threshold movement, be cycle-specific, respect explicit STOP/FINPAUSE at invocation entry, and never be cleared by a failed claim or mere helper invocation. Its service rule must prevent an ever-growing graph for the unchanged fixed-live workload. No unbounded C-local drain or blocking wait for the lease. |

The first row is the smallest source admission experiment; the second states the accounting contract required if timing still defeats it. Neither has been implemented or measured here. A combined design may be needed. The observed 64-step outer cap is not a promise of 64 committed quanta while an inner lease keeps returning zero.

A simple hard-cadence reduction, larger fixed batch, or deletion of the 50us turn is not yet a sound standalone repair. Such changes trade mutator cost against backlog without proving the relationship. Likewise, alternating SSB and recovery improves queue fairness but cannot by itself repair a total service rate below production. Optimizing away this fixture's numeric-table rescue publications would not establish general graph preservation. Keep that optimization separate.

## Required safety and progress contract

- Preserve every worker claim, exact phase/cycle hold, root snapshot/READY certificate, reader/native/recorder veto, allocation reset, quarantine grace/seal and terminal state transition. The final graph is real; no queue count or recovery identity may be discarded to manufacture completion.
- An exit request only closes entry/publishes a request while the trace owns mcode. The existing `lj_trace_exit` snapshot/TEXIT/SMR completion and `jit_base=NULL` publication must precede interpreter root/physical GC work. No main-Lua-state borrowing, finalizer dispatch from workers or root scan from an allocation helper.
- STOP and FINPAUSE keep their authoritative invocation-entry veto. A request admitted before STOP is not retroactively cancelled; STOP/restart must not be reinterpreted from a numeric threshold. Active hard-assist semantics and pending-IDLE admission stay explicit.
- Make the distinction between committed graph progress, phase completion, a lease refusal, a retained semantic owner and a failed claim visible to the scheduling decision. Do not convert attempts, counter observations or cursor rewinds into successful work.
- Bound each unit and aggregate pause; preserve a real opportunity for native execution. If a lease is the only blocker, yield at the normal boundary without polling it in a busy loop or advancing the debt as if the work completed. No synchronous sleep/spin introduced into the collector.
- In a zero-worker configuration, allocation cannot indefinitely advance the ordinary threshold while all rescue service waits for an insufficient hard-only cadence. A stable fixed live set plus repeated ordinary allocation must complete actual cycles under the existing test bound; mere exit requests, work counters, or cleanup collection do not satisfy that requirement.
- Treat worker start/stop during an active phase as a change in consumer availability. Recheck before losing a foreground obligation, and retain existing stop/join/native ownership behavior. The worker-two bridge repair and constructor fairness repair remain independent.

## Validation for a later prototype

Keep the exact original fixture hashes, 6 rounds, 32-slot live ring, 4096-allocation bursts, 64-burst (262144-table) round limit, eight real 2ms sleeps per burst, three completed-cycle target, original 45s alarm/50s outer bound, cleanup and all identity/accounting assertions. The four existing sole-main normal/strict/ASan/eb8 failures remain immutable controls.

Require the full peer0/1 x workers0/2 JIT-on matrix on normal, ROOT's exact ten-flag strict build and ASan/LSan. Also retain the JIT-off controls, STOP/restart/MAX/FINPAUSE and native-return/first-last attachment gates, compiled hard-check and closed-gate exit controls, and the existing worker/scheduler/constructor safety regressions. Engine-enabled cases with zero JIT hard checks remain that limited coverage; add separate proven compiled controls if needed, without changing the original fixture.

Measure committed SSB/recovery work, completed cycles, native entries and allocation count at real boundaries; compare paired CPU/allocation cost and bounded pause behavior before broad rollout. GDB timing is diagnostic and cannot serve as the performance or passing runtime oracle. No increase to the failing bound or explicit mutator collect/step is an acceptable substitute.
