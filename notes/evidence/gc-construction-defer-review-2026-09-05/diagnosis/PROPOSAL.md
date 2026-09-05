# Proposed narrow runtime delta — review only, unimplemented

This describes a three-source-file candidate (`src/lj_gc.h`, `src/lj_gc.c`, `src/lj_gc2.c`) against the frozen admission source. It is logically independent of automatic-control candidate3; `gc2_step_auto` will overlap that candidate's `lj_gc.c` mechanically and must be combined later in an isolated build. No code changes have been applied here.

## Concrete interface and callsites

1. Add a richer internal entry point, preserving the existing four-argument helper/API and all existing callers:

```c
LJ_FUNC uint32_t lj_gc_reclaim_gc2_arena_ex(
  global_State *g, GCArena *a, uint32_t limit,
  int *donep, int *root_owner_blockedp);
```

Rename the current implementation to `_ex`, initialize the optional result to 0 alongside `donep`, and set it to 1 ONLY in the existing acquire-observed `rootmem == LJ_ARENA_ROOT_LINKING || rootmem == LJ_ARENA_ROOT_UNLINKING` branch. Keep that branch's `pending = 1`, `cell++`, `scanned++`, and continue. Keep the exact existing scan bound, cursor writes, EOF reset, done proof, destructor/kind/lifetime/READY gates and return count. Implement the old function as a wrapper passing NULL for the new result. No shared persistent field, token, queue, allocator state or collector gate is added. The wrapper preserves existing direct fixture calls and their current numeric return contract.

2. In the single production caller (`lj_gc2_sweep_owner_progress`, `lj_gc2.c:5978`), use `_ex` with a local `root_owner_blocked = 0`. Retain remote-free draining, `step` bookkeeping, done/finish checks, PREPSWEEP cleanup, clear-pending/unseal and `gc2_sweep_reclaim_leave` in their current order. After releasing the physical writer, if the local result is set, publish `gc2_quantum_defer(g)` once and break out of the owner quantum before repeating the same arena. Do not change quarantine linkage or set done/finished because of this result. No direct return from the protected section is allowed.

If useful work occurred earlier in the same owner quantum, retain its work counters; the event prevents callers from treating the aggregate as permission to retry immediately. Pure cursor advancement through the blocked slice must not produce additional logical forward progress after the event. The exact accounting placement should be reviewed in the candidate diff; correctness rests on the explicit result and event, not on overloading zero/nonzero work counts.

3. In `gc2_worker_sweep_progress`, after the existing call to owner progress at `lj_gc2.c:22296`, stop its TG loop when either `finished` is true OR the already-sampled `defer0` differs from `gc2_deferred_epoch_acq(g)`. This respects the event's existing stop-quantum semantics rather than continuing physical work under a new deferral. `gc2_worker_drain_inner` already maps changed epoch to logical progress 0 at 22518 and reopens its gates/releases its token normally.

4. In `gc2_step_auto`, snapshot deferred_epoch around each `lj_gc2_step_explicit(L, 1)` call and break the outer automatic loop on change, before MARK yield or further automatic steps. Preserve its current debt/threshold/VM-state epilogue and sweep completion bound. Without this caller change the bounded explicit step returns on the new deferral, but an automatic batch immediately calls it again for the remaining automatic units.

No change is initially proposed for `lj_gc2_collect_active`, `lj_gc2_step_explicit` or worker main: they already observe the monotonic event around bounded quanta. Full collect returns 0; explicit steps yield with the cycle active; worker main enters its existing minimum 1ms deferred backoff. The public one-shot `lj_gc2_worker_drain` returns executed work, while logical forward progress is separate. If a new test interprets raw work as no retry obligation, fix that oracle instead of changing this broader public contract without review.

## Ownership and retained-work proof

- Detection uses the exact small-arena allocation-start root metadata that already vetoes body inspection. It makes no same-thread guess, does not require READY, and does not read an incomplete function header. It covers both legitimate constructing publishers and intrusive unlink owners.
- LINKING/UNLINKING is a negative progress result, never positive permission. The lifetime, kind, block, mark, READY, recovery, late, token/descriptor planes are untouched by the new result. The existing reclaimer is still the only source of done, and the existing quarantine finish is still the only authority for list removal and reuse.
- The existing scan/cursor behavior remains the durable retry identity. The optional result does not consume it. If a blocker lies before EOF, later bounded visits may continue scanning or finish according to the unchanged bitmap proofs. If the existing EOF pending rule resets the cursor, a later quantum restarts exactly as before. The candidate must not pin the cursor to the blocker, clear pending state, forge finish, or skip protected cells permanently.
- `func_newL_gc_base` still uniquely publishes its completed chain after all upvalues. `lj_gc_linkobj_new_chain` publishes READY/header and the pending root chain before committing CONSTRUCT/LINKING to LIVE/MEMBER (`lj_gc.c:5379`; `lj_arena.c:554`). Its exact constructor identity route deliberately avoids dependence on an unrelated registry writer and can proceed after the collector returns.
- On OOM, `func_pending_chain_free` cancels the exact private allocations. `lj_mem_freegco_unpublished` uses `lj_mem_abandon_gco_unpublished` and `lj_arena_root_construct_abandon` (`lj_gc.c:5600`, `lj_arena.c:659`) to clear its own root claim and restore the lawful lifetime before ordinary free. No collector impersonation is added.
- Existing terminal/recovery/free publication can wake SWEEP (`lj_arena.c:185-194`; `gc_root_clear_complete` at `lj_gc.c:770`). The constructor commit itself is not a general wake guarantee. Even if a particular publication does not wake, worker main's bounded active-phase/deferred timed retry remains authority, and subsequent mutator/explicit collection retries the still-active cycle. A candidate must prove eventual completion after BOTH successful publication and cancellation, not rely solely on a hypothetical wake.

## Scope of the global event and fairness risk

The event belongs to one `global_State`, not every VM. This proposal bumps it at most once per owner quantum that actually observed a protected cell, after protected cleanup; never once per bitmap cell or from idle polling. The worker token serializes physical sweep quanta, so simultaneous physical scanners cannot create an event storm by themselves. The existing epoch is an observation, not a latch: a later operation snapshots its new value and can progress. No cycle is aborted and already-committed useful work is not rolled back.

Nevertheless, a concurrent driver in the same global state may observe another driver's event and conservatively yield despite unrelated useful work being available. Stopping the TG list at the first blocked owner can also delay later TG arenas. Preserving the advancing cursor often permits later work on the next quantum, but the EOF restart geometry can repeatedly rediscover the same blocker. The narrow proposal bounds each retry and removes unbounded synchronous self-wait; it does not by itself prove starvation freedom among arenas/TGs while a publisher is indefinitely suspended.

Do not solve that by suppressing repeat events for an unchanged blocker: a later full collector samples the new epoch and would self-wait again. If focused mixed-owner validation shows harmful delay, the next source alternative is a LOCAL blocked result aggregated across a bounded/fair sweep-owner round, with a single defer publication after other independently eligible work receives its turn. That requires its own continuation/fairness design and proof; simply continuing irreversible work after publishing a global stop event is inconsistent with current driver semantics. Likewise a persistent new blocker generation requires exact publisher/cancel acknowledgement and is larger than this candidate.

## Required candidate evidence before integration

Use an isolated runtime build and preserve all original fixture predicates and bounds. Compare against the exact frozen baseline, with four/six helpers kept explicit.

- At the existing hook, show full collect returns deferred promptly while the same closure remains block-present, LINKING/CONSTRUCT, READY clear, its arena quarantined and its retry cursor preserved. The unchanged original fixture must then take its expected OOM path, leave the live open upvalue valid, and complete cleanup collection.
- Add focused successful-construction coverage with a real constructor allocation and real publish/cancel APIs: publish the held object after bounded deferral and show eventual arena/cycle completion without losing a live edge. Cancellation must independently make the same retained work drainable, with no leaked quarantine/destructor ticket. Prefer existing legitimate hook/helpers; never manufacture owner metadata.
- Exercise an automatic step and background worker while a real publisher remains suspended. Show a bounded call/backoff and retained-work identity; show owner release lets workers finish. Include useful work in another eligible arena/TG so global-event conservatism and retry scheduling are measurable rather than assumed harmless.
- A control must retain the existing owner gate and demonstrate that skipping it would make the preservation oracle fail; simply relaxing the timeout or assertion is not a repair. An unmodified-baseline timeout alongside candidate passes is useful only with actual state/return/terminal evidence.
- Run existing root pending/race, constructor/arena sweep, activation-veto, retirement and scheduler regressions appropriate to this source delta. No timeout increase, fixture graph edit, gate suppression, synthetic READY/root/lifetime publication, or broader automatic-control changes belong in this candidate.

Other source alternatives remain explicit: moving the test hook outside construction reduces coverage and masks the collector contract issue; preserving/completing small arenas like huge mappings requires broader bitmap/constructor/cancel proof. The local-result and existing-event approach is preferred for a first bounded candidate, subject to the retained-work and mixed-owner evidence above.
