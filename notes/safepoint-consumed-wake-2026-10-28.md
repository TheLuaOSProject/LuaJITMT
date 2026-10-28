# Counted request arriving before a consumed-poll wait

This is a shared LuaJITMT safepoint correction discovered while validating
the ARM64 local-write checkpoint `f39cdfe5`. Completion of the overall port
means parity with x86_64 LuaJITMT; that parity remains incomplete.

## Reproduced defect and minimal change

An owner can consume a request and claim its epoch while the leader has
already sampled its earlier acknowledgement and empty request mask. The
leader counts another request for the same epoch. The owner drops its first
pending slot, observes an empty request mask, and prepares to wait for its
consumed poll to clear. If the leader publishes and wakes the duplicate
before the owner enters the kernel wait, `poll` remains one. The old
indefinite wait can sleep through the wake: the owner waits for the leader's
clear while the leader waits for the duplicate pending slot.

`safepoint_wait_consumed_ack` now waits at most one millisecond per iteration,
matching the existing leader/pending wait cadence. Every retry rechecks the
poll and request mask and retains the original leader and trace-exit
conditions. Only an already-blocked consumed-poll waiter pays for a timeout.
The patch adds no lock, shared authority, TG field, generated-code operation,
or fast-path retry. It changes the common path on ARM64 and x86_64.

## Deterministic regression

`t-safepoint-consumed-wake.c` uses a real Lua state, a rooted coroutine claimed
by an attached owner TG on a pthread, and an actual FLUSH_SSB handshake. Test
hooks force this order:

1. The owner consumes the first request before its epoch claim.
2. The leader counts a same-epoch duplicate and pauses before publication.
3. The owner reaches the wait after its empty-request check.
4. The leader publishes the duplicate and completes its futex wake while the
   owner remains before the kernel wait and `poll` remains one.

A watchdog supplies only an extra wake after one second if progress is lost;
it never clears a poll or request, decrements pending, or impersonates an ack.
The test requires two counts, two consumed requests, two signals, the same
epoch, no early owner return, fully drained state, and zero watchdog rescues.
The before-wait and after-signal hooks compile out without GC2 test helpers.

The old indefinite-wait build fails with `rescue=1 pending=0`, after the
watchdog releases the actual blocked owner. The corrected assertion build
passes three consecutive runs with `rescue=0`. This demonstrates a real
progress failure rather than asserting a particular timeout constant.

The existing remote completion, local duplicate scanner hold, and native
root hold fixtures now recognize both timed and indefinite kernel waits.
After observing the exact poll wait, each relevant observer keeps its
scanner/action barrier held for another ten milliseconds and verifies that
the owner still has not resumed. Their ownership, poll, epoch and pending
conditions remain intact. The new progress regression catches reversion to
the indefinite wait; these older fixtures retain the safety checks.

The fixture is registered in the Darwin GC2 and Linux/x64 native completion
suites. The serial targeted matrix passes all seven profiles below, with
three controlled-race runs and all eleven existing completion/scan-hold modes
in each: 21 race runs and 77 hold checks in total. Replacing only the wait
with its old indefinite form fails the rescue assertion on both ARM64 and
x86_64 assertion profiles, using the final fixture source.

| Darwin profile | Profile hash prefix |
| --- | --- |
| ARM64 assertion | `d347a9a358dc` |
| arm64e assertion with BTI | `369b3b6883a6` |
| ARM64 release | `daa0b5f5666c` |
| arm64e release with BTI | `cb38b898c273` |
| x86_64 assertion | `f7cf76baccc4` |
| x86_64 release | `ce81f390c80c` |
| ARM64 assertion without JIT | `d70465b41520` |

The x86_64 binaries execute under Rosetta on this Mac. Linux execution has
not been established for this patch.

The complete replacement matrix passes at frozen source checkpoint
`dad7e318`: both experimental and bootstrap gates, all 26 table/FFI fixtures
in four profiles, the combined M7 scalar FFI suite in four profiles, and both
509-case stock corpora on ARM64 and signed x86_64. The newly registered race
fixture also passes inside `m3_darwin_gc2_runtime`. The ordinary ARM64 build
passes all six leaf/child/compound/bitops/cell/upvalue Lua contracts.

The source checkpoint is integrated into `v2.1`. Its independent main
rebuild passes both 509-case corpora and the same six ordinary Lua contracts,
with no checked test-helper symbols or imported compiler atomic helper.
The main and worktree executable SHA-256 is
`d8ed08ad42efe5b2eda9840652f2b16af9e0b17916b7a204ea9efa1930314517`.

## Evidence and limits

Build scripts, binaries, profile hashes and logs are retained in
`/Users/frityet/Scratch/ljmt-consumed-wake-20261028`. `baseline-run.log` records
the reproduced failure; `fixed-run.log` records the first three passing runs.
`profiles.log` is the serial cross-profile run.

The earlier rooted-insertion timeout had compatible two-thread wait stacks,
but its exact pending/request state was not captured. This regression proves
and fixes the independently forced race; it does not establish the cause of
that earlier timeout. The later complete replacement validation passes;
unrestricted ARM64 JIT behavior and x86_64 parity remain unfinished.
