# Scheduler READY authority fixture correction — frozen 2026-09-05

The corrected fixture passes on both immutable runtimes in default, assertion/APICHECK and ASan/LSan configurations. It tests READY refusal with no workers or competing TGs and a released token, then requires an actual successful close after owned READY publication. The asynchronous arena case publishes its complete synthetic setup under real worker-token ownership and permits workers to finish the phase before the test observes them.

Only the isolated scheduler fixture changed. Runtime source, admission gates, abort handling, graph protocol, all existing observational time bounds, physical arena assertions and terminal worker/TG cleanup remain. No shared file/build or same-TG rotation validation occurred.

## Patch and provenance

- Patch: `fixture.patch`, SHA256 `666cadc179e77d7ba9d543007d445e6c8c7ea682e555159c8bb06f9126889247`.
- Required preimage fixture: `e9d2173e1279088d500b7bff816dae54e260b039ad5c5d6034f738d5ad2a1a27`.
- Corrected fixture: `e029d5e3c1789b54f3c08cd950fb8a18a1761c21e8530809c6acce5f0849df36`.
- Reviewed source checkpoint: `source-manifest.json`, SHA256 `282fad1fdeaad8ff40cd4f83b4b706ad42bf8a442b325dcb5a966b53174ab0d6`; all ten artifacts reverified after validation.
- Prior diagnosis: `/tmp/lj-scheduler-worker-abort-20260905-wz2vi031/HANDOFF.md`, manifest `c761ae3765c9490ce432463a6974304329e3afc36c9b7dc52adfb3012b22b9de`. Its original canonical failure, default archive distinction, four matched new aborts, three all-stop generations and actual RESET_ALLOC/READY ownership witnesses remain frozen. This package copies its handoff, cause, setup, runtime index and manifest under `prior/`.

`SOURCE-PROPOSAL.md` records the source checkpoint before functional runs. The original uncompiled draft is retained as `initial-source.patch` and `initial-setup.json`; review added a fresh-cycle READY reset under owned IDLE initialization because the completed prior cycle can retain READY=1. No failed compile or runtime prompted that source adjustment. The final reviewed patch has not changed during validation.

The patch applies with `git apply --check --whitespace=error-all` to the exact preimage. At final verification, the shared scheduler fixture still had that preimage hash. ROOT owns shared integration and canonical suite execution.

## Runtime configurations

All six runtime source directories were reverified against their exact 225 input hashes before and after testing. No runtime was rebuilt. Archives are copied under `archives/` and hashed only. Their original build records/flags are copied under `prior/`.

| Runtime | Default archive SHA256 | Strict archive SHA256 | ASan archive SHA256 |
| --- | --- | --- | --- |
| Worker+fair combination | `0ff71ee36552489234ad9d48455b425bc26f2aeb3c46cc95ef1ae8f88ba78e75` | `58ade6fefc9225c1442e54166ab39bff0edb621ed1e29a83c7456e38113b3b64` | `b3e0e8d2d0905848485f6052a91c2af7bb9b62a76e3286765bb2637447760155` |
| eb8 / accepted fair baseline, without worker boundary scheduling | `e7e3beb9ff9f85ec932837f4162382f6c7a6701325b41b83f9642a761fd5a157` | `a700a7df774dc10430d820aa3d3912d6e2b873705ff7fd92ee312a4a32df293f` | `9e93b36f665464f03255ac0a34700f288e79be16ecd5d524768e1514e2c1dd95` |

Default fixture flags match canonical C compilation: `cc -std=gnu11 -O2 -Wall -Wextra -Werror -mcx16 -DLJ_GC2_TEST_HELPERS`, the matching include/archive, `-lm -ldl -pthread`, and the existing pthread_create/pthread_join link wrappers. Strict uses `-O2 -g` and the same ten runtime defines: LUA_USE_APICHECK, LUA_USE_ASSERT, LJ_FUNC_TEST_HELPERS, LJ_GC2_TEST_HELPERS, LJ_TAB_TEST_HELPERS, LJ_ARENA_TEST_HELPERS, LJ_TRACE_TEST_HELPERS, LJ_XSAVE_TEST_HELPERS, LJ_UDATA_TEST_HELPERS and LJ_STR_TEST_HELPERS. ASan uses clang `-O1 -g -fsanitize=address -fno-omit-frame-pointer` and those ten defines; runtime `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1`.

Every completed runtime uses `taskset -c 0-15`, the exact matching LUA_PATH, a 60-second outer bound, and disabled core dumps. Exact argv, CWD, relevant environment, time, exit status and fixture/archive/ELF identities appear in `focused/results.json`, `oracle/results.json` and `matrix/results.json`. `validation-inputs.json` records all runtime build configurations. These are complete C fixture runs; no Lua on/off canonical wrapper is claimed here.

## Completed validation

| Exact fixture / mode | Worker+fair | eb8 baseline |
| --- | --- | --- |
| Corrected default, initial focused run | 1 pass | 1 pass |
| Corrected default, main matrix | 10 passes | 10 passes |
| Corrected strict/APICHECK | 3 passes | 3 passes |
| Corrected ASan with LSan enabled | 3 passes | 3 passes |
| Corrected default, read-only boundary observers | 1 pass | 1 pass |
| Corrected default, negative-close return fault | Intended assertion failure | Intended assertion failure |
| Corrected default, positive-close return fault | Intended assertion failure | Intended assertion failure |
| Untouched default control | 3 passes | 3 passes |
| Untouched strict control | Abort: invalid borrowed IDLE transition gate | Pass |
| Untouched ASan control | Pass | Pass |

Total: 50 completed fixture runtimes. All 36 corrected positive runs pass (including four focused/observer runs); four return-only fault controls fail at their intended assertions; untouched controls have nine passes and one actual strict failure. There are no fixture timeouts, compile failures, sanitizer findings or leak findings in this correction generation. The unchanged strict failure is preserved at `matrix/combined-strict-baseline-0.stderr` and explicitly names the same IDLE handshake guard. Passing untouched repeats do not negate the prior default abort diagnosis.

## Oracle and authority witness

The diagnostic-only observers add no waits, protocol stores, callbacks or changed assertions. They read state around the real external fixture calls, and report it only at process exit. The ordinary runtime's internal READY/close calls are unchanged. The relevant records in both `oracle/*-observer.stderr` show:

1. Manual close: cycle 2, SWEEP, READY=0, root_scanned=1, workers=0, TGs=1, worker_active=0 and cycle_leader=0. Real close returns 0, leaving SWEEP and both owner words released.
2. Manual READY: worker_active=1 and cycle_leader=0 before and after the real publication; READY changes 0 -> 1.
3. Manual positive close: released worker_active=0 and cycle_leader=0; real close returns 1 and publishes IDLE. The fixture independently requires exactly one new completed-close count.
4. Async READY: cycle 5, workers=2, TGs=4, worker_active=1 and cycle_leader=0; READY changes 0 -> 1. There is no later direct READY call or active-phase reset.
5. RESET_ALLOC observation: only the existing real initial cycle-1 reset appears. Neither runtime performs a reset for the now-explicit synthetic async prepared frontier.

The return-only fault observer changes no GC state: one variant reports success for the manual READY=0 close and fails the negative assertion at corrected line 436; the other reports refusal for the READY=1 close and fails the positive assertion at line 445. All four expected failures are preserved. Together with actual 0/1 execution, these controls show that busy-token zeros and missing completion cannot accidentally satisfy the test.

## Retained behavior

The async physical assertion block (arena membership, RECLAIMED/NEEDSWEEP flags and exact sweep epoch) and terminal stop/join/slots/counts/detach/reclaim/fini block are byte-identical to the original fixture. Async MARK and WEAK function bodies and all 23 copied fixture helper files are byte-identical too; hashes and comparisons are in `final-validation.json`.

The only intermediate phase change permits an already-completed IDLE after the physical arena is observed. The original 1000 x 1 ms final drain bound remains. A real completed-close counter now verifies close progress even if completion precedes the observation; the original async progress proof and all final lifecycle checks remain. Runtime close admission and the invalid-gate abort are untouched.

This package is ready for ROOT's fixture integration review. Same-TG quarantine rotation remains a separate, source-approved but functionally unvalidated candidate at `/tmp/lj-quarantine-rotation-20260905-9xvkcbj4`.
