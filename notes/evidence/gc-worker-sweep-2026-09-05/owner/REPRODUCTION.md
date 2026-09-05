# Reproduction and fixture handoff

Use a fresh copy or extraction, because these evidence runners intentionally use fixed output names. Do not rerun into the frozen package. Source base and exact full candidate patch are recorded in HANDOFF.md. Build commands and compiler identities are in `*-build.json` and `environment.json`.

The production variants are `candidate2`, `candidate2-assert`, and `candidate2-asan`. Assertion flags are `-DLUA_USE_ASSERT -DLUA_USE_APICHECK`. ASan builds use clang, static mode, O1/g, `-fsanitize=address -fno-omit-frame-pointer` and those assertions. The helper variants append only `-DLJ_GC2_TEST_HELPERS`; no other helper macro is implied. Default GCC builds use the source Makefile's ordinary optimization. Each linked test compiles with gnu11/O2/g/Wall/Wextra/Werror and links the exact recorded static archive with `-lm -ldl -pthread -Wl,-E`. ASan fixture compilation adds the corresponding sanitizer flags. Every actual argv is preserved in a result JSON.

Set `LUA_PATH=VARIANT/src/?.lua;;` using the selected absolute source tree. ASan runs use exactly `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1`. The automatic fixture uses `RETENTION_JIT=0`. There are no sanitizer suppressions.

For the original automatic matrix run `python3 retention-check-run.py VARIANT 0-0-0 0-0-2 0-1-0 0-1-2`. The script's defaults also include explicit collection; those default extra cases are not claimed as part of this package's automatic matrix. The underlying executable argv is `EXECUTABLE 0 PEER WORKERS /absolute/path/peer-control.lua`. The first0 means automatic mode, PEER is0/1 and WORKERS is0/2. All original bounds and exact accounting assertions remain.

For the other production controls run `python3 stop-check-run.py VARIANT`, `python3 detach-check-run.py VARIANT`, `python3 consumed-check-run.py VARIANT`, and `python3 quiet-run.py VARIANT`. The earlier normal-only runners are preserved as used. Final fixture bytes are copied together under `worker-fixtures-final/`:

| Fixture | Runtime argv after executable | Link wrappers |
| --- | --- | --- |
| t-worker-bridge-stop.c | ACTION WORKERS API; ACTION0/1, WORKERS1/2, API0/1 | lj_safepoint_handshake |
| t-worker-bridge-detach.c | ACTION0/1 | lj_safepoint_handshake, lj_gc2_flush_ssb_detach, lj_gc2_scan_cycle_owner_tg_roots_native_parked, lj_arena_alloc_prepare_sweep_kind |
| t-worker-bridge-consumed.c | ACTION0/1 | lj_safepoint_handshake, lj_native_leave_tg, lj_tg_detach, lj_gc2_scan_cycle_owner_tg_roots_native_parked, lj_arena_alloc_prepare_sweep_kind |
| t-worker-bridge-quiet.c | WORKERS1/2 | lj_safepoint_handshake, lj_gc_sweep_gc2_unmarked |

ACTION0 selects RESET_ALLOC64 and ACTION1 selects SCAN_ROOTS16. API0 selects L-aware stop/join and API1 selects the no-L controller. Each fixture retains alarm30 and runner timeout35. The detach/consumed/quiet fixtures include the stop C fixture under a renamed main to share exact real-SWEEP setup and observation utilities; keep all four C files adjacent. Their C/Lua bytes contain no absolute package paths. This unchanged six-file set (four worker fixtures, original string fixture, peer Lua control) is the smallest already validated fixture handoff here; ROOT owns any later canonical registration or consolidation.

For existing protocol tests, `python3 existing-run.py candidate2-helpers` and its ASan counterpart reproduce the unchanged originals, including the two preserved fixture failures. Final clean generations are separate: `scheduler-cleanup-run.py VARIANT`, `duplicate-correction-v2-run.py VARIANT`, and `duplicate-correction-v2-forced-run.py VARIANT`. For the latter two, VARIANT can also be `baseline-helpers` or `baseline-asan-helpers`. The v2 permanent duplicate fixture patch is `duplicate-publication-v2.patch`; the forced observation schedule remains separate diagnostic coverage. `boundary-existing-run.py VARIANT` runs the unchanged recovery, public-table-rescan, leaf-publication and edge-lease sources.

The final focused acceptance set totals54 production-runtime cases plus34 helper-runtime cases:20 owner-root/completion modes,8 boundary safety cases,2 corrected scheduler cases and4 corrected candidate duplicate schedules. Additional original passes, baseline controls, first-generation corrected passes and negative witnesses remain indexed separately; do not silently aggregate generations into a single clean suite.

`verify-source.py` checks all tracked archive inputs across the nine existing source trees against `base-identity.json`, with the exact declared single-file substitutions. The recorded run passed. `artifact-manifest.json` records every top-level evidence file, all fixture-generation directories, source freezes, selected source context and runtime binaries. Complete build trees are verified by the source manifest rather than redundantly embedding every archived evidence directory in the final artifact manifest.
