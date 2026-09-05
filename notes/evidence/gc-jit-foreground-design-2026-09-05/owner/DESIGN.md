# No-worker SWEEP scheduling: source design checkpoint

This checkpoint identifies a small, durable request protocol and the exact missing service contract. It does **not** contain a runtime patch. The ownership proof for a restricted native publisher is tractable; the rule that converts committed graph work into another native allocation allowance is not complete. Implementing only threshold exits, a pending bit, or a larger batch would leave that gap.

All source references are to the immutable `base/` copy in this package: accepted `eb8a5b2f9ce2fd6128f4dbeef25b03896b81cfcd` plus worker candidate2. All 225 runtime/generator inputs matched the accepted combined tree and sampled shared runtime when copied. There were no builds, runtime tests, debugger runs, fixture edits, or runtime edits in this checkpoint.

## What the existing calls actually promise

| Call/state | Exact current behavior | Consequence for a repair |
| --- | --- | --- |
| `lj_gc.c:4134`, `gc_jit_phase_threshold_exit_due` | An open SWEEP does not exit for the ordinary threshold. | A no-worker soft exit can be introduced here, but an exit is a request, not service. |
| `lj_gc.c:4385`, open-SWEEP branch of `lj_gc_step_jit` | Moves `gc.threshold` to `gc.total + 4096`; wakes a configured pool. | Moving this number cannot acknowledge unpaid foreground work. The number is also written by other actors. |
| `lj_gc2.c:3274`, allocation checkpoint | Flushes TG-local debt, may consume the hard cadence, and can close JIT entry before the later generated GC check. | Request publication and the generated/C predicates must agree after local debt has become zero. A new obligation cannot exist only in a local byte count. |
| `lj_trace.c:6813` onward | Protected snapshot restore, FFI cleanup, SMR leave and TEXIT precede the ordinary GC handoff at 6849–6868. Normal GC exit publishes this TG's `jit_base=NULL` before `lj_gc_step`. | Keep that exact boundary. Raw allocation helpers may request an exit; they may not perform root/physical service. |
| `lj_gc.c:4159`, automatic driver | Calls the completion-only `lj_gc2_step_explicit(L,1)` up to 64 times. A changed `deferred_epoch` stops the batch. An ordinary lease refusal does not change that epoch. Active return moves the threshold even if no work committed. | The new path needs a refusal/service outcome. It must not charge all remaining attempts against one unchanged lease or claim. |
| `lj_gc2.c:22444`, worker drain | Finalizer queue splicing precedes `worker_active`. After the claim, SWEEP can refuse its lease or active JIT; otherwise it runs a mixed graph/physical quantum and reopens the gate. | Only an outcome produced while holding the exact worker claim and generation can authorize foreground payment. Finalizer/MPSC work is not a graph receipt. |
| `lj_gc2.c:22257`, SWEEP quantum | Prioritizes SSB/grey; recovery is reached only when those yield no work. A private-suffix handshake returns `1`; grace also returns `1`; later owner work has a different meaning again. | Its positive return is not a count of completed rescues or proof of graph closure. |
| `lj_gc2.c:6460`, normal close | Owns both worker and phase tokens, validates all graph/root/preparation/owner gates, performs the final flush handshake and revalidates before SWEEP→IDLE. | This is an existing genuine cycle-completion cancellation point. A single empty-queue sample is not a substitute. |

The preserved runtime diagnosis found actual service: 50 hard handoffs, each consuming 64 SSB slots, with no recovery completion in those handoffs. It did not find 50 failed exits. The final 258143 recovery identities are a separate work plane from the already-complete SWEEP preparation bridge.

## Smallest request ownership protocol

Restrict the first implementation's *new publishers* to an actually executing traced TG at an ordinary no-worker SWEEP threshold or its existing hard-exit checkpoint. Interpreter calls consume/retry the request; they do not continuously publish a fresh one. Worker start/stop is not itself a new root-scanning caller.

A separate atomic `uint32_t jit_sweep_owed_cycle` is a plausible minimum request representation: zero means absent; otherwise it names the current nonzero `gc2.cycle`. This is a scheduling obligation only. Existing SSB, recovery, root and arena structures retain every semantic locator; the word must never own an object, TG pointer or detached chain. The field is a proposal, not present in `base/`.

This particular restricted representation has an ownership argument:

1. The publisher must prove its **own** TG is executing the trace, has a valid nonzero `jit_base`, and samples SWEEP/READY/current cycle. An arbitrary observation that *some* TG is in JIT is insufficient publisher authority.
2. It release-publishes the cycle obligation before closing entry through the existing asynchronous SWEEP exit request. It does not advance the ordinary threshold as though the request were paid. Other threshold writers may still change that pacing hint; the independent request remains authoritative. It does no scan or collection while depending on mcode.
3. The existing two-sided entry protocol (`lj_gc2.c:78`) pairs gate closure and an SC fence with the entrant's intent publication/recheck. A worker can acknowledge this request only after acquiring `worker_active`, closing/revalidating the gate, and proving all JIT intents are gone. Therefore an actual traced request publisher cannot remain delayed across that acknowledgement, or across a completed cycle change.
4. With those publication restrictions, concurrent traced requests in the same cycle can coalesce. No traced publisher can install a fresh request during an owned, closed-gate, zero-JIT payment decision. If a future worker-control/native-return/interpreter publisher is added, this argument no longer suffices: it needs a revision-bearing CAS protocol or an equivalent exclusion proof.
5. Only the same owned service decision, or the exact normal completion of that cycle, may clear the request. Outer C return, STOP, FINPAUSE, accounting flush, threshold stores, worker advertisement and a failed claim may not clear it.
6. `gc2_cycle_inc_acqrel` (`lj_obj.h:3307`) **saturates**, rather than wraps. `gc2_mark_begin` rejects exhausted authority before publishing a new phase. Reusing that identity does not require inventing a wrapping 32-bit generation. The separate 64-bit thread-snapshot counter is not a replacement debt protocol.

The acknowledgement must remain inside the worker claim, before any native reopen or claim release. A by-value result can tell the outer driver whether to return, but an old outer result cannot later clear global debt or grant a new threshold after callbacks or a newer cycle intervened.

Do not reuse `jit_sweep_displaced`: it is consumed by `xchg(0)` to grant native fairness and is also written by failed generated native-entry intents. Those generated stores do not have the restricted publisher authority above. The MARK allowance, `hard_check_bytes`, and `helper_soft_limit` also have independent reset/meaning contracts.

## Required owned outcome, without changing public explicit-step semantics

Add an internal result path/sibling from the SWEEP service leaf through the automatic driver. Keep `lj_gc2_step_explicit`'s public completed/incomplete result and ordinary worker-counting wrappers intact. The proposed result must distinguish:

- Refused: lease, failed worker claim, active native/recorder, retained semantic owner, stopped/paused admission, or stale phase/cycle. No new service credit; the obligation stays durable.
- Committed graph work: exact local counts of successfully consumed SSB slots, completed grey traversal and completed recovery incarnations, with the same phase/cycle and claim retained. Keep those meanings separate.
- Other progress: finalizer queue splice, root preparation, suffix flush, grace or physical-owner progress. These can schedule normal phase progress but do not masquerade as rescue completion.
- Genuine cycle completion: the existing owned SWEEP→IDLE publication. This cancels a now-obsolete request; it is not invented graph credit.

The current `progress` pointer is insufficient. `gc2_worker_drain_inner` sets it to zero when the aggregate defer epoch changes, which is a safe conservative refusal, but its positive SWEEP value still combines the categories above. The global telemetry deltas cannot fill this gap: another actor or generation can change them after claim release.

The commit points are concrete:

- `gc2_drain_published_ssb_to_grey` (`lj_gc2.c:14887`) clears a slot and decreases its count only after `gc2_ssb_mark_one` has preserved its semantics. That commit can transfer work to grey or recovery; it need not finish a rescue.
- `gc2_drain_grey` and the SWEEP traversal loop republish RETRY/REQUEUED work before deferring. Their attempted `n` values cannot be called final completion.
- `gc2_recovery_drain_owned` (`lj_gc2.c:21812`) increments `n` for an attempted incarnation even when its leaf sets `stop_one`. Small and huge leaves can restore PENDING/REDIRTY or transfer the exact work elsewhere. A payment receipt needs the successful completion edge, not just this return value.
- The private-suffix `FLUSH_SSB` branch at 22324 returns `1` after invoking a synchronous handshake. Invocation alone is not a paid graph item or a closed graph.

If a leaf commits a real prefix and later defers, the simplest safe first receipt is conservatively zero for that invocation. Exact partial credit is possible only with local per-kind commit accounting before defer; it cannot be reconstructed from an aggregate post-return counter.

## Bounded retry eligibility and native restoration

An unpaid request must be consulted by both the C GC-safe predicate and x64 generated predicate, separately from `gc.total >= gc.threshold`. In particular, the current C fast guard `(cycle_leader != 0 && pending_auto_request())` cannot guard a new SWEEP obligation: `cycle_leader` is normally zero during SWEEP. The existing pending-IDLE request and all hard-assist checks remain separate.

The automatic invocation samples `lj_gc_auto_running`, then performs at most the existing configured bounded service budget. Its first unchanged lease/claim/native/semantic-owner refusal returns to the caller with the request intact. It does not wait for a clock deadline, spin on an owner, call the explicit driver another 63 times against that same refusal, or self-wake in a retry loop. The next **ordinary** eligible allocation/VM boundary can retry. Existing worker refusal/backoff remains unchanged.

The relevant generated callers do give the mutator a continuation:

- TNEW's `|1:` is **after** `x64_vm_gc_should_step` at `vm_x64.dasc:4513`. Its GC return at 5011–5016 jumps to that post-check label and proceeds with allocation.
- TDUP returns to its allocation label at 5036–5041.
- `ffgccheck` calls `fff_gcstep` and returns to the instruction after that call; `fff_gcstep` does not loop on the predicate.
- C `lj_gc_check`/`lj_gc_check_fixtop` perform one helper call.

An early progress message incorrectly inferred that TNEW's target preceded its check. The resolved label proves that inference wrong; no TNEW retry-loop counterexample was run or established. No per-instruction retry-token change is justified by that mistaken inference.

Native entry/exit still needs an explicit policy while the request remains unpaid. `asm_gc_check` currently observes only ordinary threshold and hard cadence; `asm_xpoll` observes the existing binary gate. `lj_gc2_jit_needs_exit` must preserve the reason through the normal snapshot/TEXIT path if other accounting changes the pacing number. Merely setting an invisible pending word cannot force a currently running loop out. Conversely, blindly keeping the shared gate closed until all debt disappears would change native fairness and the meaning of a refusal. That is one of the unresolved policy choices below.

Normal snapshot restoration and SMR/TEXIT completion precede service. Profile-pending, HOOK_GC and protected error exits can skip the immediate automatic call; they must retain the request for a later normal eligible boundary. `vm_exit_interp` restores interpreter execution and clears the TG JIT intent on its ordinary path. A request must never be consumed merely because an exit was recorded or a snapshot helper was called.

These are bounds on attempted/logical quanta, not a wall-time or wait-free claim. Existing root EOF complete-flush, full chain traversal, registry scans and synchronous handshakes remain outside a fixed wall-time bound. This design does not implement the separately frozen pending-root continuation proposal.

## Control and lifecycle transitions

| Transition | Required treatment of unpaid work | Source boundary / limitation |
| --- | --- | --- |
| STOP completed before automatic invocation | Retain request; veto new automatic service. Do not infer control from MAX or clear the request to avoid checks. | `api_gc_setlogical` publishes STOPPED before threshold stores. Internal running requires both STOPPED and FINPAUSE clear. |
| STOP during an already admitted invocation | Existing invocation-entry guarantee remains; no claim of cancellation of work already authorized. A later invocation must resample. | Current `gc2_step_auto` samples `running` once. New per-unit policy must not silently claim a stronger cancellation barrier. |
| FINPAUSE; nested/throwing finalizer; STOP or RESTART in callback | Retain request across saved threshold restore and protected cleanup. A nested resume clears only its owned pause; STOP survives; RESTART does not clear FINPAUSE. | `lj_gc.h:283–333`, `lj_gc2.c:5425–5458`. Public ISRUNNING continues to read explicit STOP only. No old outer receipt may acknowledge later work. |
| RESTART | After normal threshold publication and STOP clear, a retained request becomes eligible even if a delayed attachment/finalizer store leaves MAX. | New C and generated predicates must consult the obligation without the numeric/IDLE guard. |
| Worker enable advertises `n_workers>0` | Retain foreground obligation. Advertisement alone is not service or a valid transfer of ownership. | `gc2_worker_start_count_locked_l:2451` publishes count before allocation, pthread creation, actual start or its rollback. |
| Worker disable, failed creation or failed join | Retain foreground obligation. Actual worker service under the same claim may pay it; stopping/joined state may not. | STOP is published while count remains positive. Failure can retain `n_workers=unjoined` with STOP set. Zero publication happens later. |
| Count changes without an existing obligation | The narrow proposal only promises issuance at the next ordinary no-worker native allocation check. It does not promise progress if no actor reaches a later safe boundary. | A stronger last-worker handoff would add a non-JIT publisher at several control exits and require a different publication proof. Do not casually add it to the restricted native-only field protocol. |
| Successful same-cycle service | Acknowledge only using the eventual payment rule while retaining worker claim, closed native admission and exact cycle. | No threshold publication outside that authority can count as payment. |
| Normal cycle completion | Clear/cancel this cycle's request at the existing owned SWEEP→IDLE publication before native reopening. | The final close already retains both authority tokens and revalidates after the handshake. |
| Preserve abort or stale outer frame | Preserve pending semantic work; do not apply a receipt from the old phase/cycle. Before-READY abort cannot be a normal publisher of this SWEEP/READY-only request. | `lj_gc2_cycle_to_idle:6600` routes an established SWEEP bridge through normal close. MARK/WEAK/pre-READY abort has its own retained-work gates. |

Configured workers and secondary Lua mutators are independent dimensions. Zero workers does not imply sole-main ownership. Every existing reader, native, recorder, finalizer, TG, phase, preparation and arena admission gate remains required.

## Exact unresolved changes before a runtime patch

1. **Payment amount/endpoint.** Define what native allowance a request purchases. Clearing after any positive result is unsound accounting. Even a genuine 64-slot SSB commit can leave all recovery work outstanding; those are transfers, not completed rescues. A 4096-byte allocation allowance has no demonstrated universal conversion to a bounded number of graph jobs: stores into existing containers and traversal can publish work without proportionate fresh allocation. The preserved workload itself has 258143 real recovery identities after 3200 SSB transfers.
2. **Receipt propagation.** Add the exact leaf outcomes above to `gc2_worker_sweep_progress` → `gc2_worker_drain_inner` → an internal automatic sibling of `lj_gc2_step_explicit`. Decide how partial progress plus defer is retained/credited, and how finalizer/bridge/close work routes without pretending it pays graph debt. No new authoritative debt write may be deferred until outer callback return.
3. **Native turn while unpaid.** Choose whether committed prefix service earns a bounded allocation turn, or whether interpreted execution continues until an owned graph-closure certificate. Keeping the existing 50us lease but forgetting unpaid work repeats the observed failure; deleting the lease is a different fairness policy. Keeping the gate closed for the whole remaining cycle is a concrete conservative fallback, but can withhold JIT indefinitely behind a semantic owner and is not the requested preservation of native turns.
4. **Graph-closure alternative is not free.** `lj_gc2_ssb_empty` includes recovery, detached consumer ownership, grey and all live TG private suffixes. Normal close additionally checks root/NEEDSCAN/marks, preparation and physical work, then repeats checks after a handshake. A new “graph paid” certificate needs the correct subset plus a no-lost-publication proof; sampling only head/recovery counters is insufficient. Reusing full close for every soft check would also inherit synchronous/full-flush work and change cost substantially.
5. **Worker transition scope.** Retaining existing debt through enable/disable is sound and small. Guaranteeing service on the final worker's departure even without a subsequent allocating mutator is a stronger scheduling promise. It would need a non-JIT publisher/driver route at the no-record early return, post-join zero publication and all create rollback paths, with no main-Lua-state borrowing. The restricted request proof deliberately does not cover such a new producer.

The recommended next implementation scope is the restricted native-only cycle request plus an exact owned result path, with the allocation-credit/native-turn rule chosen together. The more conservative full-cycle interpreter fallback should be reviewed as an explicit policy alternative, not smuggled in as a threshold optimization. No runtime patch is attached because items 1 and 3 determine the meaning and lifetime of the request; implementing the mechanical field and predicates first would create an unpaid-work protocol without a sound payment contract.

Later validation must preserve the original automatic JIT-on and JIT-off bounds, peer0/1 × workers0/2, exact normal/strict/ASan archives and control/finalizer/worker safety cases. An actual service oracle must show committed recovery and completed cycles as well as trace execution and bounded mutator opportunities. None of that validation was run in this source-only checkpoint.
