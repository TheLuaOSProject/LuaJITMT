# Unchanged automatic workload with JIT enabled

The combined worker candidate passes 9 of 12 JIT-enabled automatic workloads. The remaining three cases (peer 0, workers 0 on normal, strict and ASan) return the original `INCOMPLETE_AUTO` status at round 4's 262144-table limit. The matched normal eb8 baseline reproduces that failure with **byte-identical stdout**; its peer 1, workers 0 control passes. No source, fixture, bound, cleanup or failure oracle was changed. No runtime patch is proposed by this validation.

## Inputs and scope

ROOT package: `/tmp/lj-worker-bridge-combined-20260905-bz9wysjp`. Its archive HEAD is `4b4ed7c239fd1f8e30369836492896677c65756e` (test cleanup atop runtime `eb8a5b2f9ce2fd6128f4dbeef25b03896b81cfcd`), combined with worker candidate2. ROOT's `setup.json` records only `src/lj_gc2.c` changed among its 225 runtime and generator inputs. `provenance/root-setup.json` is an exact copy.

All 225 expected source inputs match each of candidate, strict and ASan before and after these runs: 1350 comparisons, no mismatch. Existing linked executables, archives, fixtures and all paths in each original `identities.json` match their recorded size/hash; the executable copies used here are byte-identical. ROOT's exact build and original compile records are copied under `provenance/` and referenced in `setup.json`.

The normal baseline tree is `/tmp/lj-gc-pending-root-design-20260905-blju2qsh/baseline`; its exact reused archive path is `/tmp/lj-gc-pending-root-design-20260905-blju2qsh/baseline/src/libluajit.a` (SHA256 `cbc7e955549f291850dd5693dce77ce1d1f56461ced87eacc77e069603880343`). This is the cbc7e955 eb8 default-archive generation identified by ROOT; the earlier fair-candidate benchmark archive beginning e7e3be is not used. Its 225 runtime inputs match ROOT's `before_inputs` before and after the two controls (450 more comparisons), and its archive matches the original baseline build record. The unchanged fixture was linked in this new evidence package. Its compile command and dependencies are preserved separately.

Normal has no helper defines. Strict and ASan preserve ROOT's exact ten defines: `LUA_USE_APICHECK`, `LUA_USE_ASSERT`, and `LJ_{FUNC,GC2,TAB,ARENA,TRACE,XSAVE,UDATA,STR}_TEST_HELPERS`. ASan retains Clang `-O1`, `-fsanitize=address -fno-omit-frame-pointer` and `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1`. No test hooks are called by the unchanged automatic fixture.

| Build | Static archive SHA256 | Executable SHA256 |
| --- | --- | --- |
| candidate | `0ff71ee36552489234ad9d48455b425bc26f2aeb3c46cc95ef1ae8f88ba78e75` | `2d938364875daf8344df4e5cc66e76eeab1f4a31f1224eb4206435c03776154e` |
| strict | `58ade6fefc9225c1442e54166ab39bff0edb621ed1e29a83c7456e38113b3b64` | `47e5bab1d510a089902726bd33becc200d8985c45612aa42e1263955ea1ff624` |
| asan | `b3e0e8d2d0905848485f6052a91c2af7bb9b62a76e3286765bb2637447760155` | `6585f3016235261ec714dd7ad37067991f36cdcbd41d547602a9ba2f47fe6b2e` |
| eb8 normal baseline | `cbc7e955549f291850dd5693dce77ce1d1f56461ced87eacc77e069603880343` | `da2762397f59fdd73903e15c1ff7c5767bf64e210b1fa2ed85902e95a7ff5437` |

Exact fixture hashes:

- `fixtures/t-string-retention.c`: `2e8e840fb4ba3a3b09168c06d828ff10ebafd41e9ff555b9737f34384fea3cf9`.
- `fixtures/peer-control.lua`: `519ebf714b0a33b9a436d3452a153a1bc3eea3322ebf0bf74d8e76fea4ab8cb2`.

## Matrix

| Build | peer 0 / workers 0 | peer 0 / workers 2 | peer 1 / workers 0 | peer 1 / workers 2 |
| --- | --- | --- | --- | --- |
| normal combined | INCOMPLETE_AUTO, round 4 | PASS, 6 rounds | PASS, 6 rounds | PASS, 6 rounds |
| strict combined | INCOMPLETE_AUTO, round 4 | PASS, 6 rounds | PASS, 6 rounds | PASS, 6 rounds |
| ASan combined | INCOMPLETE_AUTO, round 4 | PASS, 6 rounds | PASS, 6 rounds | PASS, 6 rounds |
| eb8 normal baseline | INCOMPLETE_AUTO, round 4 | outside requested control | PASS, 6 rounds | outside requested control |

All failures return exit 2 through the original fixture. No external timeout, alarm termination, assertion abort, ASan/LSan report or stderr occurs in these 14 runs. The nine combined passes each complete at least three actual SWEEP-to-IDLE transitions in each of six measured rounds; completed-cycle count goes from 4 to 22 before cleanup. The baseline peer control does likewise. These are single bounded executions, not a performance benchmark or exhaustive concurrency proof.

## Preserved failure

All four sole-main failures have the same logical endpoint:

- First three rounds complete, with completed counts 7, 10 and 13.
- Round 4 executes exactly 262144 further escaping table allocations; cumulative filler count is 286720. The fixed live ring remains 32 tables.
- Expected completed count is 16; observed count is 14, in SWEEP (`phase=3`) of cycle 15. Major root scans are 45, no worker exists, and cycle leader/worker active are both zero at the snapshot.
- Published allocation debt is 26849992, trigger 393216, hard limit 786432, next hard check 27078504, and local debt 19760. JIT gate is open and `jit_hard_checks=50`. This records substantial production allocation and some completed cycles; it does not identify the retained sweep object/owner or establish infinite nonprogress.
- The original cleanup completes explicit sole-main cycles (completed count 17), verifies canonical identity and payload of all 32 anchored strings, reclaims all 16384 churn strings, restores the exact initial string count 300, verifies empty string-reclamation state, and closes normally. Cleanup is preserved as cleanup; it is not counted as successful automatic completion.

Normal combined and eb8 baseline failure stdout are byte-identical, SHA256 `85d679d62c651545ff2656baf16b982a34ed305499d19f460913988c4a3b2678`. The strict/ASan outputs differ in small byte-accounting values but match the above logical counts. This particular JIT scheduling failure predates the worker bridge. Its cause remains undiagnosed here and is not attributed to the separate canonical scheduler abort.

The nine successful combined cases retain all 24576 churn strings during automatic collection under the existing string policy, then recover all 24576 during original sole-main cleanup and restore string count 300. No RSS-based reclamation claim is made.

## JIT coverage limit

`RETENTION_JIT=1` is the only behavioral input changed from ROOT's original automatic matrix. The fixture successfully enables the JIT engine initially; its `jit_enabled` JSON field reports that requested choice, not a repeated `jit.status()` query. The failing sole-main schedules reach 50 real JIT hard checks, whose production increments require the JIT path/published JIT base (`jit-counter-source.json`). Thus these failures exercise active compiled allocation scheduling.

Every other case reports zero JIT hard checks. Those are valid engine-enabled workload passes, but this fixture does not establish that they exercise the same compiled hard-check path. Zero counters do not prove that no native trace executed. No threshold or hot-loop adjustment was made to force coverage.

## Reproduction and artifact layout

`results.json` contains all 12 combined argv, working directories, controlled environments, 50-second external bounds, 45-second internal alarm, process status, elapsed time and new stdout/stderr paths. `baseline-results.json` contains the two matched controls. `final-summary.json` contains exact endpoints and per-case counts. `run.py` and `baseline.py` are the executed drivers; reproduce them only in a new package, because this evidence package is frozen.

The per-case command is `taskset -c 0-15 EXE 0 PEER WORKERS PEER_LUA`, with `RETENTION_JIT=1` and the exact ROOT tree `LUA_PATH` recorded per row. Non-ASan runs remove `ASAN_OPTIONS`; ASan uses the option string above. `RLIMIT_CORE` is zero. The original fixture's native waits remain eight real 2ms sleeps per 4096-allocation burst, at most 64 bursts per round, targeting three actual completed cycles. No explicit collect/step is introduced into measured automatic allocation. Original initial collection and final cleanup remain unchanged.

The C and Lua fixture bytes contain no absolute package paths. Drivers, dependency records, provenance and recorded commands intentionally identify absolute evidence paths. Frozen input packages and shared worktree/builds were never edited. Pending-root continuation design remains frozen and unimplemented.
