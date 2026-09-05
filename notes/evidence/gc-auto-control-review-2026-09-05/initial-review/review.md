# Independent review: pending automatic GC admission v1

Verdict: do not integrate this frozen candidate. Its new C and x64 checks reach existing GC-safe boundaries correctly, and they address the demonstrated lost admission after allocation accounting flushes. However, they expose a STOP violation during first-child attachment. The owner has now reproduced that exact source interleaving against production normal archives: control preserves 4 starts/4 completions, while the candidate starts MARK 5/4 after STOP completed. A repair and new overlap controls are required.

This is a source review. I did not build or run this candidate. Runtime observations below are attributed to the owner's preserved artifacts, which I inspected. No shared source or existing evidence was edited. The review concerns admission to the existing synchronous collector; it does not establish automatic cycle completion, asynchronous GC, string-body reclamation with peers, or a nonwaiting collector.

## Frozen identity

- Base: `597b8705208957ade8465416da30976ab9b52195`.
- Owner package: `/tmp/lj-gc-auto-admission-20260905-h7ntx71p`.
- `candidate.patch`: SHA256 `f632c7f26051ffebb55fde65291a304b91a46df0371cf47f86aa1783efdf5cd9`.
- `src/lj_gc.c`: `d0e8608a261b823f21b037aa73f4f29dac3059ee3e741286d836c686e7498e9d`.
- `src/lj_gc.h`: `f465a7706da2f3f7fc52f0b40054462d0a6368c3e5dec0abcd9d990c6680cfd5`.
- `src/vm_x64.dasc`: `d445a4ffa214242bd5efc753c3cb737e90509b66c9f52bd95eff4c358b6e77ab`.

The three candidate source files and their controls are copied here. `reviewed-inputs.json` records the other source identities used for the proof. Line references below name the frozen candidate tree, not a subsequently changed shared checkout.

## What the admission change fixes

`lj_gc2_account_alloc` can flush TG-local allocation debt and publish a nonzero `cycle_leader` while the phase remains IDLE. This is deliberately a request: raw allocation accounting cannot synchronously collect with an incomplete Lua stack. The flush can leave both the TG-local cadence and the GC2 hard threshold below their next check. With a live secondary, the global threshold is MAX, so the old interpreted boundary checks miss the durable request.

The additional `lj_gc_should_step` condition (`lj_gc.h:409`), x64 macro checks (`vm_x64.dasc:415`), and `gc_step_assist_top` driver admission (`lj_gc.c:4291`) connect that durable request to an existing safe driver call. The predicate performs loads only: it does not clear a leader, mutate a phase, manufacture a request, or claim a native/reclamation certificate. A normal missed concurrent publication remains retriable because the token persists. The predicate alone does not provide the authority to consume a request.

The nonzero check also accepts `LJ_THREAD_GCSCAN`, which can appear briefly with legacy IDLE during close/reset. This is a harmless extra driver attempt: `gc2_worker_claim` (`lj_gc2.c:1494`) rejects GCSCAN both before and after its CAS, and `gc2_mark_begin` (`4104`) checks it again before phase effects. The automatic loop breaks after the failed unit leaves IDLE; it does not run its whole batch against this sentinel. Filtering to `lj_thr_id_is_owner` is an optional cost cleanup, not a required correctness repair.

## Safe VM and C boundaries

The x64 change is confined to the existing `x64_vm_gc_should_step` predicate. It uses its existing scratch register and branches to the old slow targets; it neither calls C itself nor changes frame layout. The local labels introduced in the macro resolve internally and preserve the three caller targets.

- Fast functions: the macro at `vm_x64.dasc:1590` still enters `fff_gcstep` (`2719`). That path publishes PC, `L->base`, and argument top; preserves the native return address; calls `lj_gc_step_top`; then reloads the possibly relocated stack and reconstructs the argument count.
- TNEW: the macro at `4524` still enters the pre-construction `lj_gc_step_fixtop` path. BASE and PC are already saved; allocation operands are reconstructed and BASE is reloaded before publishing the table result. There is no unrooted new table across this check.
- TDUP: the macro at `5035` follows the same pre-construction discipline. The template comes from rooted prototype constants after the check, and BASE is reloaded after the allocation call.
- `lj_gc_check`/`lj_gc_check_fixtop` keep their old stack contracts. `lj_gc_step_fixtop` reconstructs the current frame's top before the common driver. Function construction uses `func_fnew_preserve_operands` before that boundary (`lj_func.c:1151`), and new open-upvalue construction checks before creating its unrooted cell.
- The inspected C call sites already collect on the old threshold path: number-to-string API conversion checks before allocating the unrooted string, concatenation publishes its result in the Lua stack before checking, FFI call return publishes its result and leaves/checks the native state before collection, and trace completion checks after terminal recorder ownership release.

The candidate does not move collection into raw allocation accounting, a native leaf, or an FFI wait. Existing native entry/exit and mark gate authority remain in force. `gc2_mark_begin` closes JIT entry and, if any TG is still JIT-active, releases its worker token while retaining the exact request for a later safe retry. The owner's native-return control establishes real `lj_native_enter` execution and that ordinary `threading.sleep` return itself does not consume the request; the next allocating boundary does.

No additional root/stack geometry blocker was found in this scope. This does not replace build/assert/ASan checks or proof of every unrelated allocation helper.

## Blocking first-child STOP race

`gc_logical_running` (`lj_gc.c:66`) and `gc2_logical_stopped` (`lj_gc2.c:2837`) infer the logical state from one of two thresholds, selected by `mt_live`. Their second live-count sample covers some last-child transitions, but it is not an atomic publication protocol.

`threading_gc_enter_counted` (`lib_threading.c:885`) increments `mt_live` from zero to one before it copies the global threshold into `mt_gc_threshold`. No automatic-admission exclusion covers this interval. `mt_entering` is held, but neither logical reader nor `gc2_step_auto`/`gc2_mark_begin` rejects it.

The minimal source interleaving is:

1. After an earlier child generation, `mt_gc_threshold` retains a finite running value while `mt_live == 0`.
2. Ordinary allocation publishes a real IDLE request and consumes its local debt. `LUA_GCSTOP` completes in the sole-main world. `api_gc_setlogical` (`lj_api.c:2963`) writes only the global threshold to MAX in this state, leaving the old finite MT value intact.
3. An external thread performs real `lj_threading_attach`. Pause it immediately after its first `mt_live_add_rlx`, before the threshold copy. The visible state is `live == 1`, `entering != 0`, global threshold MAX, and old finite MT threshold.
4. An ordinary TNEW boundary on the main thread sees the pending token and falsely reads the old MT threshold as logically running. `gc2_step_auto` repeats the same false-running read.
5. The automatic driver calls `lj_gc2_step_explicit`, whose explicit request/admission path intentionally bypasses public STOP. `gc2_mark_begin` has no logical-stop or entering exclusion. A new MARK cycle begins after the completed STOP.

The old threshold-only TNEW control stays below the accounting/hard thresholds and does not enter this path. Thus the threshold publication defect was already present, but the unwanted automatic MARK in this bounded case is a candidate regression. `LUA_GCISRUNNING` already reports the wrong transient value in both versions; that query defect must be labeled separately.

The owner reproduced the exact interval with a Linux x64 TF/SIGTRAP observer around real external attach. The fixture changes no phase, request, threshold, live count, or native certificate. It single-steps production instructions and pauses after the real live increment; an observer releases that thread after a real cycle-start counter change or completion of the one main-thread call. This release condition lets the unchanged synchronous handshake complete instead of manufacturing a hang.

Evidence: `/tmp/lj-gc-auto-stop-overlap-20260905-y4h4cc8a/t-stop-first-attach.c`, `overlap-results.json`, and the control/candidate stdout copies under this package's `overlap/`. The real request was published with local debt 0 and `since=302340 < hard=583456`. The pause occurred 365 instructions after arming. The control exits 0 with 4/4 cycles; candidate exits 42 after entering MARK 5/4. Both clean up normally. This reviewer inspected, but did not execute, those runs.

## Repair constraints

A bounded explicit-stop veto is the smallest clear safety repair for the reproduced STOP case. Public STOP must release-publish an independent atomic veto before changing pacing thresholds. Thread attach/detach, cycle pacing, and finalizer save/restore must never clear that veto. Automatic logical readers must acquire it at the actual driver decision, not only in the new fast predicate; request publication should honor it too. In particular, `gc_step_assist_top` calls `lj_gc2_check_trigger` before the driver, so `gc2_logical_stopped` also needs the veto to prevent fresh requests from a stale MT value after STOP. `lj_gc2_assist` itself accepts only MARK/WEAK and cannot start an IDLE cycle. Explicit `step`/`collect` retain their current ability to work while automatic collection is stopped. Public restart and the successful full-collect restart path must deliberately clear it after their restart publication. Applying the same veto to `LUA_GCISRUNNING` would repair the transient false query answer without redefining its remaining threshold semantics.

This is a veto proposal, not a fully proved implementation or complete logical-state redesign. In particular:

- A before/after `mt_entering == 0` check is insufficient. The counter can do 0→1→0 around a stale threshold sample, and an entrant can start after the predicate check but before the driver's independent read. No timeout or advisory snapshot provides publication authority.
- Writing both thresholds in STOP is insufficient. A first attacher can capture the old finite source before STOP and overwrite a target with that stale value after STOP writes MAX.
- `helper_soft_limit == UINT64_MAX` cannot be reused as the authority. `gc2_reset_alloc_trigger` (`lj_gc2.c:3400`) writes MAX on every ordinary cycle start, even while automatic GC is enabled; idle publication can also leave it MAX under the global MT bridge.
- A veto alone does not repair restart progress. A first attacher can capture a stopped global MAX, a concurrent RESTART can publish finite MT and clear the veto, and the delayed attacher can overwrite MT with MAX. The old logical reader then remains false even though the explicit veto is clear. This is a distinct derived-threshold publication problem that needs a controlled overlap test and must not be claimed fixed by merely adding the veto.
- Overlapping public STOP and RESTART also need an explicit ordering contract. If the last authoritative atomic action determines the logical state, older in-flight pacing/bridge stores must not override that decision. A canonical atomic logical state independent of pacing thresholds, or a correctly versioned publication protocol, avoids that class of stale-store problem. It is broader than the three-file admission candidate.
- Preserve temporary internal finalizer suppression independently. `gc2_finalizer_pause_threshold`/`restore_threshold` (`lj_gc2.c:5220`) use global MAX as a temporary callback pause and preserve a saved MT value; the callback latch has separate nesting and spawn semantics. Replacing all threshold checks with a single public running flag would silently alter these internal paths and direct-threshold helper tests unless those contracts are migrated deliberately.
- A STOP veto does not make the existing automatic driver wait-free or cancel an admission already in flight. If STOP is required to exclude even an already-authorized but not-yet-published MARK after its return, its publication must arbitrate with automatic admission itself. A standalone sampled flag is not such a barrier. At minimum, a STOP that completed before the automatic call must reliably veto that call, as in the reproduced regression.

A broader single authoritative logical state plus separate pacing thresholds is easier to reason about than deriving logical intent from whichever threshold `mt_live` currently selects. For this bounded candidate, keep the negative v1 evidence and validate the smallest explicit safety veto separately; document remaining publication/progress limitations rather than silently treating it as the full redesign.

## Owner evidence and remaining validation

The copied `summary.json` records normal/assert/ASan controls. The observed steady-state result is useful: with zero GC workers, automatic interpreted allocation with a live peer now completes the requested 18 cycles in the existing filler bound. The six low-debt boundary controls exercise TNEW, TDUP, fast string allocation, C string allocation, API number conversion, and FNEW. Sequential STOP/restart and last/first child controls preserve the request until restart; they do not force the first-live publication interval above.

The candidate does not solve worker-pool completion. With two GC workers, the bounded automatic cases can start two cycles but finish only one before remaining in SWEEP at the 262144-filler limit. The explicit string-retention baseline also remains: live peers or a worker pool prevent the separate physical string-body reclaim authority; dropping/then fully collecting in a sole-main world recovers those bodies. Neither behavior is changed by this admission patch.

Some helper failures are matched controls, not newly established candidate regressions: `t-gc2-interp-hard-check` and `t-gc2-alloc-account` assert under both builds; `t-jit-idle-reclaim-entry` times out at 60 seconds under both. Initial Lua helper invocation failures from a missing LUA_PATH have later correctly configured passing runs. They must remain visible in the owner's report, with their exact commands and exit statuses.

Before integrating a repaired candidate, require the production first-live overlap control to stay stopped, the same request to survive and be admitted after a real restart, and the existing six boundary/active-STOP/native-return controls to pass on the exact combined source. Include the reverse bridge and captured-MAX/restart overlap where the intended repair claims them. Repeat meaningful normal/assert/ASan coverage after the repair, preserving the old failed candidate and matched baseline results. Do not describe the present worker2 bounds, source review, or first-cycle admission as complete automatic/asynchronous GC.
