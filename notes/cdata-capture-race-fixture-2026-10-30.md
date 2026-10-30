# Cdata `__call` capture race fixture

The M7 race fixture previously gave `run_scalar_metacall_race` eight calls after warming a different sibling helper. It had no signal for whether the exact helper entered the recorder, sampled a function, consumed the hook, or reached the VM pause. A missed seam therefore surfaced only as the generic 10-second timeout.

The fixture now retries the exact race helper at most eight times. Each attempt flushes traces and resets `hotloop=1`/`hotexit=1`. Forward and reverse helpers run 64 calls per attempt; the nonfunction helper runs its existing eight-call protected loop. A retry stops as soon as the test hook is consumed. The original 10-second parent seam deadline and the forward, reverse, and nonfunction mutation assertions remain in place. A timeout aborts both hooks, releases a worker if it is paused, and reports recorder entry/sample/consume plus VM capture call/match/pause counts. No trace observer is used to drive the race.

The recorder counters require both `LJ_TRACE_TEST_HELPERS` and `LJ_GC2_TEST_HELPERS`; the VM counters use the existing `LJ_GC2_TEST_HELPERS` guard. Recorder counts distinguish entry from a successfully sampled Lua function and from consuming the test hook. VM counts distinguish capture hook invocation, matching the exact target cdata, and entering the pause. A separate abort helper disables future pauses and releases an already paused worker so a failed test can join cleanly.

## Validation

Ran `tools/ci/arm64_cdata_capture_race_contract.sh` on branch `codex/cdata-race-deterministic`, based on `d0ee5eb8c4249a46e1e11350281f325e78a00bd9`. The contract built the test-helper runtime for arm64 and arm64e, with assertions and without assertions. Under each profile it ran the plain Lua companion once, then the C-hook race harness three times (12 race-harness runs total). All runs passed. Each C run asserted exactly one recorder consumption, one target VM match, and one pause for each of the forward, reverse, and nonfunction cases. Retries were exercised: arm64 release required a second attempt for one nonfunction case, and arm64e assertion required a second attempt for forward and nonfunction cases. Those first attempts did not consume the recorder hook; the retry reached the exact seam.

The first validation attempt exposed a fixture expectation mistake: before the parent changes `__call`, the nonfunction case's `pcall` correctly succeeds. The retry expected this pre-seam success; the post-mutation error check remains after the pause. The final run passed.

Final corrected run log: `/Users/frityet/Scratch/ljmt-cdata-capture-race-20261030/four-profile-corrected.log`.

The checked-in runtime build was restored to the default ARM64 assertion profile by the profile helper after the contract. The source remains based on the exact `d0ee5eb8c` tree; this task makes no production admission or runtime behavior change.
