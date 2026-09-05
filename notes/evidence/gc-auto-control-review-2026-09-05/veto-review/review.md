# Independent source review: explicit automatic-GC STOP veto

Verdict: no source blocker found for the bounded veto repair. It prevents the demonstrated first-attachment admission from overriding a public STOP that completed before the automatic call. It preserves the old threshold rules as additional restrictions. Exact-source runtime validation remains the owner's responsibility; this review performed no builds or runs.

This is a new immutable package. It does not revise the failed original candidate or its review at `/tmp/lj-gc-auto-admission-proof-20260905-mrosx7pd`. The original candidate's VM/C safe-boundary proof and its synchronous/progress limitations still apply.

## Reviewed identity

- Owner tree: `/tmp/lj-gc-auto-stop-overlap-20260905-y4h4cc8a/veto`.
- Veto-only delta SHA256: `4808e3c7b435db176df4e5c4c33ce3bf8729cd3bfa6d5d2f1efd0a7f7c967048`.
- Full patch against `597b8705208957ade8465416da30976ab9b52195`: `35e8abddecefe17ca842d96d4f2f237fd986303ca3eac8b45c1f60d2610f542b`.
- All six reviewed files are copied under `src/`; their hashes and four other inspected input hashes are in `reviewed-inputs.json`. The owner's existing build result is copied for provenance, not counted as reviewer validation.

## Publication and admission proof

The delta renames `GCState.unused0` to `auto_stopped`, retaining its one-byte type and position. It therefore does not grow GCState or shift subsequent offsets. A production-source search found no former `unused0` access or wider adjacent-field store that can overwrite this byte. `lj_gc2_init` explicitly initializes it to zero; its only production caller is `lua_newstate` after zeroing the new GG_State and before publication. No reset-at-cycle-end path clears the veto.

`lj_gc_auto_stopped_load` and `store` use the existing atomic byte acquire/release helpers. Public STOP publishes 1 before any pacing or threshold stores. A nonthrowing restart/full-collect restart path publishes its thresholds first and then clears the byte. `api_gc_setlogical` performs no fallible operation between these publications. Attach/detach and temporary finalizer threshold bridges never write this field.

The new flag is a veto, not a replacement for threshold-based pacing or internal suppression:

- `gc_logical_running` checks it before choosing a threshold. This covers both `lj_gc_pending_auto_request` and the independent logical-running read at the start of `gc2_step_auto`. A stale finite MT threshold after completed STOP cannot authorize either call.
- `gc2_logical_stopped` also checks it. This matters because `gc_step_assist_top` invokes `lj_gc2_check_trigger` before the automatic driver's check, and raw allocation accounting can also publish requests. Automatic request creation therefore respects the explicit STOP even while the threshold bridge is stale.
- The x64 pending-request branch checks the byte after observing a nonzero leader and IDLE. The C driver remains authoritative. Old total-threshold/hard-cadence branches can still enter the slow helper, but they cannot bypass the driver's veto to start an IDLE cycle.
- `LUA_GCISRUNNING` checks the byte before the old threshold selection, fixing the demonstrated transient true result after completed STOP. It retains all other old threshold behavior.
- Explicit `LUA_GCSTEP`/`LUA_GCCOLLECT` do not pass through this automatic admission veto. An explicit step can work while the veto stays set; the existing successful full-collect API return deliberately restarts automatic collection. A throwing collect returns before `api_gc_setlogical`, so it does not accidentally clear STOP.

The existing hard-assist path accepts only MARK/WEAK, and therefore cannot consume an IDLE request behind the veto. Existing GC workers/assists may continue already-active work after STOP, as before. The patch adds no helper call, allocation, lock, wait, counter retry, new request authority, or native-entry bypass.

The first-live counter can now change or ABA freely without defeating a completed explicit STOP: neither the selected threshold nor a first/last-child stale store can clear the independent veto. The relevant fast paths read it atomically; the x64 byte load follows the existing TSO acquire convention.

## Limits that remain open

This is intentionally not a complete logical-state publication redesign. A delayed first attach can capture global MAX before RESTART, then overwrite the new finite MT threshold after RESTART clears the veto. The old threshold checks can consequently remain false despite the flag being clear. That restart-liveness defect needs separate overlap evidence and repair. Overlapping STOP/RESTART calls likewise retain derived-threshold ordering issues. The veto does not claim to resolve them.

`gc2_step_auto` samples logical running at the start of an invocation. The veto is not a cancellation barrier for a call already in flight when STOP occurs, nor an atomic arbitration with MARK publication. The reviewed guarantee is that a STOP which completed before an automatic call vetoes that call; it does not cancel previously authorized work.

Preserving the old threshold checks preserves temporary finalizer pause behavior and raw-threshold controls when the explicit flag has never been set or has been cleared normally. Internal tests that call public STOP and later write a finite threshold directly cannot use that raw store to clear the new explicit authority; such a fixture must perform a deliberate public restart if it intends to resume automatic GC. New test failures require individual inspection, not blanket relabeling.

The original candidate's limitations remain: the driver is synchronous, two-worker automatic runs still miss their completed-cycle bounds in SWEEP, and physical string-body reclamation with a live peer or configured worker pool remains separately excluded. This patch should be described as pending-request admission with explicit STOP preservation, not complete asynchronous/automatic GC.

## Required validation

Validate the exact copied source against the production first-live overlap fixture, including unchanged pending-request identity/counters while stopped and normal cleanup. Then restart through the public API and establish real admission. Repeat the six allocation-boundary controls, sequential first/last-child STOP/restart, active-STOP, and real native-return controls under the intended normal/assert/ASan variants. Keep the original candidate's failing overlap and control result immutable. The six-file combined patch must be verified again when rebased onto the actual integration HEAD; source review alone is not evidence that that combined executable ran.
