# Worker SWEEP preparation, candidate2

Candidate2 makes configured workers invoke the existing SWEEP preparation boundary and measures its outcome under the same worker claim and cycle. It also closes each worker's startup native scope before private detach. The original fixed-bound automatic workload now completes with two workers, including while a secondary Lua thread remains alive. No string reclamation policy is changed.

This is an isolated implementation for ROOT review and later combination. It uses runtime base **79345529bd932e68f8159ec17224467a10cad09b**, archived when shared docs HEAD was e3b2ec6afc4f6819fad7fad84dc179c250196155. It does **not** contain the separate fair-constructor repair subsequently committed as eb8a5b2f. No shared sources, tests, builds or commits were changed by this package.

## Exact source

Only `src/lj_gc2.c` changes. `candidate2-full.patch` is the complete delta against base793, SHA256 **5f59dcef6d97461c4475ba8852c84714b51a99f2c22f19a58a8d574beb136116**. The frozen file `source-v2/lj_gc2.c` is **582259b42e29e34f616763a8c4b565361a3798a8089871c3e1cd71519c313afb**. `candidate2-incremental.patch` contains only the native-exit repair relative to frozen candidate1, SHA256 **72c1636f1a73c8f2887e0b5e7b4505590772cb605a971bb8c40543cbc2279346**.

The old public void boundary wrapper and every existing caller remain. Its internal helper retains the exact phase, recovery, finalizer, root, JIT, reader, SSB, NEEDSCAN and READY guards. It returns BLOCKED, PROGRESS, COMPLETE or DEFERRED before releasing worker ownership. A real cursor/unlink/EOF/certificate advance schedules another unit without needing an external wake. An unchanged frontier or defer enters the existing minimum backoff and reabsorbs retry self-wakes.

`SOURCE-PROOF.md` covers admission, progress, native ownership and shutdown. The parent reviewed candidate1's actual source and the native-close direction before focused validation; candidate2 is delivered for final review. Source verification checked all **9,129 archived tracked files in each of nine build trees**: 82,161 checks, no missing or unexpected differences, allowing only the declared candidate1/candidate2 file substitution. See `source-verification.json`, `base-identity.json`, and the two source-freeze manifests. Base archive SHA256 is ab743eb7b10098d76ccfedf460838e2b7018fef0f15747f64a1f117d3b334766.

## Original counterexample and matched control

The prior immutable diagnosis remains at `/tmp/lj-gc-worker-sweep-20260905-2xv7dsqc`; its handoff/proposal were copied without change under `previous/`. It distinguished early real graph work from late empty graph/private queues with unfinished ownership-spine pruning and uncopied pending roots. The final late samples had no LINKING/CONSTRUCT object, active reader, quarantine item or finalizer hold. Existing workers never scheduled the missing preparation boundary. This is separate from the constructor-owned quarantine defer diagnosis.

The exact earlier `t-string-retention.c` and `peer-control.lua` were reused. It runs JIT-off automatic allocation with a fixed 32-slot live ring, 4,096-table bursts, eight real 2ms native pauses per burst, at most 64 bursts (262,144 tables) per round, three actual completed cycles per round and six rounds. It does not insert explicit collect/step into measured automatic allocation. Bounds remain a 45-second alarm and 50-second subprocess timeout.

The matched unchanged base793 runtime passes both worker-zero controls, but both worker-two cases return **2 at the original 262,144-table bound**, with SWEEP active and zero measured completed cycles (starts5/completed4). Candidate2 normal/assertion/ASan variants pass all **12** peer0/1 × worker0/2 cases, completing 18 measured cycles each. Normal filler totals are 49,152 for worker-zero, 57,344 for peer0/worker2 and 65,536 for peer1/worker2. These are completion evidence under this workload, not a throughput benchmark or a general progress guarantee.

All 32 anchored strings keep canonical identity. Exact string counts and byte accounting remain in the output. The 24,576 churn strings remain retained while the active peer/pool collection restrictions apply; sole-main explicit cleanup reclaims them. No RSS inference or concurrent string-reclamation claim is made.

## Candidate2 focused validation

The primary matrix uses production runtimes without test-helper defines. Observation-only linker wrappers hold real operations; they do not write synthetic runtime phase, request, native, root or certificate state. Except for the unchanged automatic workload, setup uses the real explicit MARK/WEAK/SWEEP transition functions to reach the target protocol state.

| Control | Normal | Assertions | ASan + assertions |
| --- | ---: | ---: | ---: |
| Original automatic allocation: peer0/1, workers0/2 | 4 | 4 | 4 |
| Pending real RESET_ALLOC/SCAN_ROOTS, workers1/2, L-aware/no-L stop/join | 8 | 8 | 8 |
| New request after native close and detach's last poll | 2 | 2 | 2 |
| Already-consumed remote action held before native close | 2 | 2 | 2 |
| Quiet native completion and unchanged-claim backoff, workers1/2 | 2 | 2 | 2 |

**54/54 primary runtime controls pass.** All ASan runs enable `detect_leaks=1:abort_on_error=1`, with no suppression. Compilations are not counted as runtime passes.

The already-consumed controls observe the real worker exit at native depth exactly1, then native0/poll1 with the remote action held for 20ms. RESET already owns the epoch; SCAN is held before its epoch claim. Neither may enter detach until the real action completes. The later-request control starts the real worker handshake after the target is DETACHING and has passed its own last poll. With native0, the action remains owner-pending and does not overlap private teardown; join drains the protocol normally.

The quiet control publishes 1,024 real tables into a 32-slot ring while a worker's first pruning call is paused under its claim, then leaves the main caller native with no further allocation or mutator collection until the same cycle completes. During the held claim, exact cursor/pending-head identity, completion and async progress stay unchanged; the second worker parks19 times over about20ms. After release, all builds observe six pruning calls, five cursor-only advances, 203 actual unlinks, one EOF and one real completed cycle. The remaining32 payloads are checked. This demonstrates claim-refusal backoff and actual quiet worker progress; it does not force every internal same-cursor validation failure.

Additional existing source controls use a separate runtime with exactly `LJ_GC2_TEST_HELPERS` plus assertions, and its ASan counterpart. Native-root-hold modes0..3 and remote-root-completion modes0..5 pass20/20. Unchanged recovery, public-table-rescan, leaf-publication and edge-lease fixtures pass8/8. Table-coalescing was not run in this package because it also needs `LJ_ARENA_TEST_HELPERS`; no mismatched-helper run is claimed.

## Preserved failures and separate fixture generations

Candidate1 is immutable. Its caller-side stop matrix passes8/8, but the unchanged detach overlap witness fails both RESET and SCAN with exit2 after normal cleanup. A worker remained native1 while DETACHING, and reached RETIRED/terminal actor state before an already consumed remote action returned. This demonstrates private-state exclusion failure, not a witnessed UAF. Candidate2's exact unchanged witness passes in all three builds. See `DETACH-WITNESS-V1.md` and `detach-v1-manifest.json`.

The first stop fixture incorrectly required zero actions during worker startup: a preceding real RESET may return64. Its four SCAN assertion failures remain under `stop-fixture-v1/` and their output names. The second version omitted polling a preceding RESET before waiting for target SCAN entry; two ten-second waits remain under `stop-fixture-v2/`. Final v3 polls only preceding startup work and withholds every target action until stop/join begins. The original failed fixtures and bounds are not overwritten or counted as passing controls.

Two unchanged existing-fixture failures also remain. They have separate diagnoses and fixture-only corrections:

- Original candidate2 GCC local-native-duplicate failure asserted reqmask0 at the existing consumed/pre-claim hook. A bounded observation-only pause reproduces the same invalid zero-only oracle on unchanged baseline and candidate2 using the real same-epoch publication. Corrected v2 requires nonzero to equal the exact expected actions, the published epoch to equal the callback epoch, and the published actions to match. Every later completion/teardown oracle remains. Ordinary and exact forced schedules pass8/8 on matched baseline/candidate2 assertion/ASan sources. `DUPLICATE-V2-HANDOFF.md` and `duplicate-v2-manifest.json` freeze all identities and preservation details. Only `duplicate-publication-v2.patch` is the final proposed correction; the first, less strict correction remains archived.
- Original candidate2 ASan scheduler reports a 131,280-byte/six-allocation leak. The fixture leaves its synthetic `mt_shutdown=1`, causing the real close claim to refuse. ROOT's other agent independently traced the exact real close and froze a fixture cleanup patch. That accepted patch is copied under `existing-cleanup-v2/`, SHA256 67b54d179d8a6fb4e95920662344c7737828bf1f86d47977c350367eb2b55f23. The separate corrected fixture passes candidate2 assertions and ASan+LSan2/2. The original leak output remains; no suppression or redundant baseline leak rerun was used here.

## Scope and remaining limits

This resolves the demonstrated worker preparation omission and its newly exercised exit lifetime requirement. It does not establish lock freedom or wait freedom. Handshakes remain synchronous; a noncooperating actor can delay them and a stop/join already inside one must finish its existing ownership protocol. Root EOF still flushes whole detached pending chains. A preempted claim holder, persistent parser/constructor owner, finalizer, JIT/reader gate or unsafe publisher can still defer progress. The worker never borrows mainL, invokes a finalizer or creates a new IDLE request through this change.

The candidate intentionally classifies some graph-only or head-to-head pending flush work as BLOCKED and takes the backoff. This can delay work conservatively. Source proof covers unchanged/deferred outcomes; the quiet runtime control specifically forces a busy claim. The independent fair-constructor patch and all combinations require separate validation. No Windows/macOS or release claim is made.

`REPRODUCTION.md` gives exact runner selection, fixture arguments and compile/environment conventions. Every exact invocation, result and binary identity is preserved in the corresponding JSON/stdout/stderr files. `result-inventory.json` indexes all historical and final results without collapsing generations. `artifact-manifest.json` hashes the handoff, source, fixtures, old counterexamples, logs, result files and linked binaries; `source-verification.json` verifies the complete archived inputs for every runtime tree.
