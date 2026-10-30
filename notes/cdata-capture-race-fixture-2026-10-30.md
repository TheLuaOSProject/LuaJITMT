# Cdata `__call` capture race fixture

The M7 race fixture previously gave `run_scalar_metacall_race` eight calls after warming a different sibling helper. It had no signal for whether the exact helper entered the recorder, sampled a function, consumed the hook, or reached the VM pause. A missed seam therefore surfaced only as the generic 10-second timeout.

The fixture now retries the exact race helper at most eight times. Each attempt flushes traces, resets `hotloop=1`/`hotexit=1`, and runs 64 helper calls. A retry stops as soon as the test hook is consumed. The original 10-second parent seam deadline and the forward, reverse, and nonfunction mutation assertions remain in place. A timeout aborts both hooks, releases a worker if it is paused, and reports recorder entry/sample/consume plus VM capture call/match/pause counts. No trace observer is used to drive the race.

The recorder and VM counters are compiled only with `LJ_TRACE_TEST_HELPERS` and `LJ_GC2_TEST_HELPERS`. Recorder counts distinguish entry from a successfully sampled Lua function and from consuming the test hook. VM counts distinguish capture hook invocation, matching the exact target cdata, and entering the pause. A separate abort helper disables future pauses and releases an already paused worker so a failed test can join cleanly.

## Validation

Ran `tools/ci/arm64_cdata_capture_race_contract.sh` on branch `codex/cdata-race-deterministic`, based on `d0ee5eb8c4249a46e1e11350281f325e78a00bd9`. The contract built the test-helper runtime for arm64 and arm64e, with assertions and without assertions, then ran the race harness three times per profile (12 runs total). All runs passed. Each run asserted exactly one recorder consumption, one target VM match, and one pause for each of the forward, reverse, and nonfunction cases. One arm64 release nonfunction case required two cold attempts; the first attempt returned without entering the cdata recorder, and the next reached the exact seam. This demonstrates the retry covers the previously undiagnosed missed-entry stage.

The first validation attempt exposed a fixture expectation mistake: before the parent changes `__call`, the nonfunction case's `pcall` correctly succeeds. The retry expected this pre-seam success; the post-mutation error check remains after the pause. The final run passed.

Final run log: `/Users/frityet/Scratch/ljmt-cdata-capture-race-20261030/four-profile.log`.

The checked-in runtime build was restored to the default ARM64 assertion profile by the profile helper after the contract. The source remains based on the exact `d0ee5eb8c` tree; this task makes no production admission or runtime behavior change.
