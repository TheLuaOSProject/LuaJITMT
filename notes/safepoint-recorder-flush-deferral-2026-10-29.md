# Recorder acknowledgement before trace flush — 2026-10-29

A real `EXIT_TRACES|FLUSHJ` handshake could deadlock when it arrived while the
local recorder owned the JIT token: the owner waited for the leader's consumed
poll, while the leader's full trace flush waited for that token. The regression
fixture pauses a real root-side publication, attaches a distinct peer TG, and
starts the normal handshake. It does not manufacture handshake state.

The consumed-poll wait now defers only for the exact local TG while it owns the
token and `EXIT_TRACES` has left the recorder either asynchronously aborted or
at the irreversible `PUBLISH` state. In the first case, `trace_state` consumes
the normal protected `RECERR` path; in the second, it completes the publication
suffix. Both paths release the token in their existing state machine. The wait
helper leaves the consumed poll armed, so a later interpreter or native-return
boundary still waits for the leader's global action. `FLUSHJ` by itself does
not qualify, and remote acknowledgers never inspect another TG's recorder.
The same wait helper serves first acknowledgements, claim losers, and the
actions-zero consumed-poll retry.

The focused ARM64/arm64e first-side production contract passed with the real
append/flush race and existing sibling retirement checks. The source contract
checks that the exact recorder states defer the wait without clearing poll.
The ordinary recorder fixture passed the focused attached-peer FLUSHJ case on
x86-64/Rosetta with profiling disabled for that targeted run.

Evidence is under `/Users/frityet/Scratch/ljmt-multiside-20261029/deadlock-checkpoint/`:

- `final-arm64-contract-2.log` — ARM64/arm64e first-side production contract.
- `x64-build/final-build.log` and `x64-build/final-flush-only.log` — focused
  x86-64 build and protected recorder FLUSHJ fixture.
- `final-arm64-contract-2.log` includes the final profile restore and contract
  success line; `jit_recorder_safepoint_contract.sh` also passed after the last
  source change.
- `fixed-contract-rerun.log` records an intermediate SIGSEGV from the earlier
  destructive `lj_trace_abort_owner_before_park` experiment (process 18441).
  No matching diagnostic report was produced; that experiment was discarded
  and is not evidence about the final wait-only fix.

The broad x86-64 M6 attempts did not finish. The profiling-enabled run stopped
at the existing `test_profile_side_pre_admission` snapshot-count assertion;
the profiling-disabled run passed the new flush test and then stopped at the
existing `test_real_counted_before_poll_side_admission` snapshot-count
assertion. The focused x86-64 case is the architecture-specific evidence for
this change; it is not a full x86-64 suite result.
