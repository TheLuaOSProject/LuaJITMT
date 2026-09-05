# Function construction / full-collect timeout diagnosis

Frozen Linux-only review, 2026-09-05. No shared source, fixture, build, or runtime candidate was edited. This is independent of automatic-control candidate3 and of the separately blocked attachment fixture. The completed scheduler fixture fix is separately frozen at `/tmp/lj-gc-scheduler-publication-20260905-2zckpem_`.

## Finding

A real unlinked raw closure is owned by the constructor when the existing test hook calls the production `lj_gc2_collect_active`. In captured runs the nested collector reaches SWEEP with empty logical queues, but repeatedly scans the same quarantined small arena while that exact closure remains LINKING / CONSTRUCT / not READY. The constructor cannot publish or cancel it until nested collection returns. The reclaimer preserves its owner correctly; the full collector does not receive a deferral and treats repeated cursor work as forward progress.

This is a production collector progress-contract problem exercised by a test-only full-collection injection. Ordinary production `func_finduv_nothrow` has no corresponding explicit full-collection call in this interval. Do not generalize this reproduction into a proven ordinary Lua trigger or modify the fixture merely to suppress it. The narrow source proposal is in `PROPOSAL.md`; it has not been implemented or tested.

## Exact configurations and observations

The fixture is unchanged everywhere: `tests/t-func-construction-anchor.c`, SHA256 `42e9d47359fb27565565fe07678abbc8de7abf119226075cab3a1142768a6f0f`.

Four-helper flags:

```
-DLUA_USE_ASSERT -DLUA_USE_APICHECK -DLJ_GC2_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS -DLJ_TAB_TEST_HELPERS -DLJ_FUNC_TEST_HELPERS
```

Six-helper flags append:

```
-DLJ_UDATA_TEST_HELPERS -DLJ_STR_TEST_HELPERS
```

- Original six-helper pristine-597b control, initial admission candidate, and STOP-veto candidate all timed out at the unchanged 60-second outer bound. Original three-second debugger interruptions place each inside the injected nested full collect at fixture line 193. Raw results and debugger logs are preserved under `prior/` from `/tmp/lj-gc-auto-stop-overlap-20260905-y4h4cc8a`.
- The earlier exact four-helper admission executable passed originally and passed this agent's one native rerun in 0.015944 seconds. `four-helper-control.json` records exact argv, CWD, LUA_PATH, original 60-second bound, and identities. Archive SHA256 `89b9ecfc5e7b4ea489d530ba6a887f98e846eb5d3ad8bd43851705674a499cef`; executable SHA256 `1a2e8252bc2b5656da9a002263c83b4e20f1bfdb1a6962495f979802c61eea5c`. A fresh exact relink produced the byte-identical ELF; it was not separately run. Both disassemblies are preserved, with differing filename labels only.
- The fixture discards the nested full-collect return and accepts a changed cycle OR an active phase. Therefore a passing fixture does not prove the nested full collector completed. Its exact historical/internal return path was not captured. No claim that the four/six macro difference causes the timeout is justified.
- Independent, later four-helper evidence from the string agent distinguishes the original passing admission executable from an exactly matched previous-veto control and automatic-control candidate2. Both later binaries time out at 60 seconds in the untouched fixture. Three-second interruption stacks remain at the same line-193 nested collect. The control stack is in sweep bridge prepare; the candidate stack is in small arena reclaim. Copied exact results/identities/build records/logs are under `prior/string-agent/`. Their source package is `/tmp/lj-gc-auto-control-20260905-qs673ryl`, evidence manifest SHA256 `5bdc2b9431e12a3d769d579b2ab09da3e68074b72258650b9c1fe81274351300`. These are distinct runtime variants, not reruns of the original passing executable.
- New private four/six debug builds use the original admission sources, the exact matching helper macros, and `CCDEBUG=-g TARGET_STRIP=:`. Existing `-O2 -fomit-frame-pointer` is unchanged. All 807 input files match each private build and frozen source, verified in `final-source-verification.json`. The only build change is debug metadata; debugger observations are not claimed to be the original binary or native timing.
- All five debugger observations (initial six, v2 six, four-observation, construction-six, construction-four) were deliberately interrupted three seconds after the entry marker. Their fixture schedule/predicates/hooks were not edited, no GC state was written, and no inferior helper function was called. GDB exit 0 means successful evidence capture, never a fixture pass. Exact compile/debugger argv, environments, binary hashes, startup/interrupt bounds, elapsed times, and capture flags are in each `debug-results.json`.

The source and fixture inputs of the original four/six admission builds are identical. Their original `lj_func.o`, `lj_gc.o`, and `lj_gc2.o` are byte-identical despite the two extra helpers in other translation units. `frozen-inputs.json` retains the exact source/archive/ELF identities; final verification rechecks them.

## Captured state and its limits

The initial debugger generation could not recover optimized-out `fn`. Its snapshot error is preserved. In `v2/`, `tail` at `i == 0` is used only because source establishes `tail = obj2gco(fn)` before any pending-chain append. The later constructor-return FinishBreakpoint independently captures the actual returned function pointer before its first upvalue allocation.

In both `construction-six/debug.stdout` and `construction-four/debug.stdout`, constructor return and nested collect entry have identical geometry and GC state:

| Field | Value |
|---|---|
| Phase / cycle / handshake epoch | IDLE 0 / 0 / 0 |
| Cycle leader / workers | 1 / 0 |
| Function / arena / start cell | `0x7ffff7a592f0` / `0x7ffff7a50000` / 2351 |
| Function block / mark / READY | 1 / 0 / 0 |
| Function root / lifetime | LINKING 1 / CONSTRUCT 2 |
| Function recovery / late / sweep | 0 / 0 / 0 |
| Arena owner / flags / sweep epoch / retire epoch | main TG 1 / 9 / 0 / 0 |
| Allocator prepare / sweep epochs | 0 / 0 |
| Allocator owned arena | that exact arena |
| Needsweep / quarantine | null / null |
| New open upvalue at collect entry | same arena, cell 2355, READY 1, LIVE 1 |

The initial six `v2` run and four-observation run both reach SWEEP cycle 1, handshake epoch 11, roots scanned 1, bridge ready 1, grace needed 0. SSB head/drain/consumer are zero, grey top/bottom are 5/5, recovery is empty/not failed, table/thread pending counts are zero. No background worker threads exist; the main collector owns worker/exclusive-writer state while reclaiming.

The exact function remains block 1, mark 1, READY 0, LINKING 1, CONSTRUCT 2 in the main TG's quarantine. Arena flags are 89 (TRAVERSABLE | REGISTERED | QUARANTINE | PREPSWEEP), retirement epoch 9, allocator prepare/sweep epochs 1/1. Reclaim cursor is partway through the arena. `sweep_owner_runs` reaches 843243 (six) and 844833 (four) in roughly three seconds, arena completions remain zero, and `deferred_epoch` remains zero. This connects the actual allocation to the actual repeated physical sweep target; it is not an invented GC state or an inferred pointer.

The two richer constructor observations also install a source-line breakpoint intended for the LINKING branch. Optimized line mapping moves it to the following `rootmem == MEMBER` line (3029), so it never reports the targeted refusal branch and greatly slows execution (120/125 owner runs). These runs prove constructor/entry state and later quarantine identity only. They do not constitute a captured exact branch execution. Their logs and this limitation are retained; no additional diagnostic run was used to manufacture the missing event.

## Source chain

Line references below are to the exact reviewed admission source copies under `reviewed-source/`, not the evolving shared tree.

1. `lj_func.c:893` creates an unlinked raw function through ROOT_CONSTRUCT allocation. Its structural ownership exists before READY/header publication. `func_newL_gc_base:1075` allocates it before the upvalue loop, initializes `tail` to it, and only publishes the complete chain at 1136. OOM cancels/frees the private chain at 1144-1147.
2. The existing `func_finduv_nothrow` hook at `lj_func.c:85-86` calls full collect after publishing a legitimate open upvalue but while the enclosing function is still incomplete. Fixture line 193 arms this hook and later injects OOM on the second upvalue.
3. `lj_gc_reclaim_gc2_arena`, `lj_gc.c:2983`, reads allocation metadata under its existing quarantine/activation/physical-owner gates. At 3020-3027, LINKING or UNLINKING means a publisher/remover owns the intrusive link and reuse veto. The collector sets local `pending`, skips the cell, and does not inspect mutable header bytes or alter ownership. This safety rule is correct and must remain.
4. At 3303 onward, a call reaching EOF with local pending/deferred/gcprep work resets the cursor to FIRST_CELL. Non-EOF cursor movement returns positive work even without a state change. `lj_gc2_sweep_owner_progress:5862` repeatedly calls the 64-unit scanner at 5978, and the bounded owner/worker quanta can repeatedly return cursor work without reducing the unresolved owner frontier.
5. Full collect, `lj_gc2_collect_active:2935`, already returns deferred for several same-owner/external ownership dependencies and for `deferred_epoch` changes. In the captured path no event changes, so positive logical drain sends the full collector back around indefinitely. Worker main similarly continues positive steps unless the event changes; it has an existing bounded deferral backoff. `gc2_step_auto`, `lj_gc.c:4156`, calls step_explicit(1) repeatedly without its own epoch comparison, so a new lower-level deferral must also stop this outer automatic batch.
6. `pending` is LOCAL TO EACH 64-unit scan. A LINKING cell encountered in a call that reaches EOF forces restart. A LINKING encounter in an earlier call can be forgotten before a later EOF call; the bitmap finish/apply retains root-owned spans and can complete that arena. This is a concrete bounded-scan/arena-geometry hypothesis for why some binaries/runs pass, not a demonstrated explanation of the historical pass. The matching debug four/six geometry does not distinguish them.
7. Huge reclaim, `lj_gc.c:3378`, explicitly preserves a root-owned mapping and permits sweep progress, leaving reclamation for a later cycle. Extending small-arena completion similarly would need separate terminal bitmap and constructor/cancellation proof. It is broader than forwarding an existing deferral event while retaining every current gate.

## Recommendation and remaining proof

Review the local-result / existing deferred_epoch proposal before edits. A successful correction must preserve the exact owner, arena quarantine, cursor retry identity, mark/READY/kind/lifetime/token planes and all finish authority. Then demonstrate prompt nested deferral AND post-publication/cancellation eventual collection, using the unchanged fixture and focused retained-work checks. More fixture passes alone are insufficient. Repeated global epoch events may conservatively stop other work in the same global state; this needs explicit fairness/throughput evidence, described in `PROPOSAL.md`.
