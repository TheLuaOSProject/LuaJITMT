# Real pending-root EOF cost and bounded follow-up design

One real SWEEP pruning invocation processes all 262,144 pending objects in the long-chain fixture. The healthy source performs whole-chain tail and overlap walks plus full-spine repair, so the advertised256-entry prune batch does not bound EOF work. A resumable whole-chain continuation is the preferred next direction; no production patch is proposed until shared complete-flush ownership, lifetime and reentrancy are resolved. Arbitrary prefix cutting conflicts with an unfinished constructor's next-link reads.

This fresh package uses exact runtime **eb8a5b2f9ce2fd6128f4dbeef25b03896b81cfcd**. Shared HEAD at creation was 4b4ed7c239fd1f8e30369836492896677c65756e. The worker-bridge candidate, same-TG arena rotation and iterator work are not combined. No shared files/builds or any runtime source were edited. Verification hashed all 10,611 archived tracked files in each of the normal and ASan trees: 21,222 matches, no missing or changed input. The archive SHA256 is 7380e69db335cf8a526ac4916ccb48c439f7772b5834678350429a4e8b4bed3e.

`PROPOSAL.md` contains the source trace, alternatives, required continuation state, full-consumer contracts, cancellation/EOF/READY obligations and the exact boundaries of the helpability claim. `caller-contexts.json` freezes 40 flush/repair callsite contexts, including table retired-storage presence checks and trace owner searches that must not miss a continuation.

## Measured production unit

`t-pending-root-eof.c` SHA256 is **0e2cf3e8dda8d43c39a6b2ee9dd2c4bc24fffd057751ae983b95d1a7579e31cd**. It uses ordinary Lua allocation calls, no worker/helper macros or synthetic phase/request/root state. It reaches SWEEP through real transition functions, completes the initial real allocator/root preparation, then creates a fixed number of ordinary tables, userdata or a mix. A fixed 1,024-table prelude preserves a non-EOF starting cursor; a 32-slot ring is the measured allocation's live set. Actual barrier/recovery work is drained separately with the production worker-drain function, and an exact counter assertion proves that setup does not flush pending roots.

An observation-only linker wrapper times the actual `lj_gc_sweep_gc2_unmarked` invocation. It verifies the same phase/cycle and worker claim before and after, and uses the production pending-root-flushed counter. The count and identity walks occur outside timing. With a nonzero chain, that one EOF call flushes exactly the requested N, leaves EOF/READY false, and restarts the existing pruning frontier. It does not complete the cycle by pretending the new roots were already visited.

Every target object is checked exactly once before and after transfer, with a separately allocated pointer-identity map. Table values and userdata payloads are verified; every userdata is after the permanent main-thread anchor. A later explicit production collection must complete actual cycles and retain all 1,024 prelude payloads and the final 32 ring payloads. RSS is not used. This is a real single-threaded boundary control of EOF, not a new demonstration of asynchronous worker execution.

| Pending objects / lane | Normal EOF time | ASan + assertions EOF time |
| --- | ---: | ---: |
| 0 | 14.134 microseconds | 25.305 microseconds |
| 4,096 ordinary tables | 78.310 microseconds | 183.715 microseconds |
| 262,144 ordinary tables | 5.412454 milliseconds | 11.106938 milliseconds |
| 262,144 after-main userdata | 5.032717 milliseconds | 9.985332 milliseconds |
| 262,144 mixed | 5.780674 milliseconds | 11.022468 milliseconds |

Final matrix: **10/10 runtimes pass**, with ASan `detect_leaks=1:abort_on_error=1` and no suppression. Each number is one observed invocation, not a stable latency distribution or a throughput comparison; normal and ASan processes were run concurrently. The structural absence of a fixed per-unit bound does not depend on these timings. All large cases publish exactly 262,144 identities and leave 262,149 root nodes at the observation point. The mixed case verifies 131,072 userdata after main. A normal large EOF unit also unlinks 204 earlier root entries before the whole flush, versus the preceding three 256-entry prune units.

`eof-measurements.json` records the values, `baseline-final-results.json` and `baseline-asan-final-results.json` record exact commands/bounds/statuses, and corresponding input manifests identify each archive and test binary. The normal runtime archive is cbc7e955549f291850dd5693dce77ce1d1f56461ced87eacc77e069603880343; ASan is 9d48cb6e3ef391de797c2082c0a87c103116470d6b1f54bd549d59d7e440a264.

## Reproduction and preserved setup attempts

Use a fresh copied package, not these frozen outputs. `python3 build.py baseline` builds default GCC. `python3 build.py baseline-asan` extracts the exact archive and builds static clang/O1/g with assertions/APICHECK and AddressSanitizer. Build JSON records the exact Make argv and generated binaries. `environment.json` records compiler/tool versions.

From the copied package, run `EOF_RUN_SUFFIX=-new python3 run.py VARIANT 0-0 4096-0 262144-0 262144-1 262144-2`, with VARIANT baseline or baseline-asan. The executable arguments are COUNT MODE: mode 0 ordinary tables, mode 1 userdata after main, mode 2 alternating. Counts are bounded at 262,144. The fixture alarm remains 45 seconds and each subprocess timeout 50 seconds. Its EOF-admission loop remains 64 boundary calls. Each fixture compiles gnu11/O2/g/Wall/Wextra/Werror, links the exact archive with `-lm -ldl -pthread -Wl,-E`, and wraps only `lj_gc_sweep_gc2_unmarked`. No fixture C bytes contain an absolute package path; the runner resolves its directory and sets LUA_PATH. Final fixture/runner bytes are also copied under `fixture-final/`.

Initial fixture attempts remain separate and are not counted in the final matrix. V1 incorrectly required an internal integer-tag representation for values produced by `lua_pushinteger`; the value is numerically correct in the default runtime's number representation. V2 fixes that oracle but omitted real recovery drain after the allocation burst: its 4,096 case hits the unchanged 64-call setup bound without entering the prune helper. The preserved debugger observation has SWEEP, recovery_items=3961, recovery_failed=0, table_rescan_pending=1, grey top/bottom=4 and worker_active=0; this is existing graph work legitimately excluding root preparation. The debugger also records an unsuccessful request for a nonexistent ssb_index member. V3 attempted to call a file-static SSB helper and did not compile. V4 reads the actual global/private SSB publications, drains real graph work through production calls, proves no root flush occurred during setup, and retains all old bounds. V4's small controls and the final matrix pass. All old source versions, compiler failures, binaries and outputs remain archived.

The proposed follow-up is bounded scheduling, not permission to skip unfinished roots or turn ownership metadata into semantic liveness. Full consumers retain complete-flush contracts. A continuation must be discoverable and lifetime-safe at every release, count real progress, preserve constructor links, and veto EOF/READY until complete. The public source currently supplies no ready-made descriptor protocol with those properties. No runtime change is ready to integrate from this package.
