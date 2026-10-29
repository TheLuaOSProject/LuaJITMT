# Recorder safepoint deferral: combined action coverage

The protected-recorder flush fixture now runs the real attached-peer handshake
with `EXIT_TRACES | FLUSHJ | SCAN_OWNER_ROOTS | FLUSH_SSB`. A test-only signal
seam pauses the leader after it publishes both the owner's request and poll
bit. The recorder consumes its abort and releases the JIT token while the
leader remains paused. At the next owner boundary, the fixture observes the
consumed-poll wait with `reqmask == 0`, the recorder idle, the token released,
and the same combined mask still globally published. It then releases the
real leader and verifies the handshake completes.

This exercises the `actions == 0` consumed-poll path after the deferral. It
does not synthesize an epoch or request mask. The active-recorder claim-loser
path is not forced here: making a second same-TG acknowledgement race the
owner's epoch claim would require an artificial concurrent owner ACK. The
existing local-native duplicate test covers claim-loser/duplicate root-scan
behavior without an active recorder token.

Focused `flush-only` validation passed on macOS ARM64 and x64/Rosetta with
strict `-Wall -Wextra -Werror` fixture compilation. The ARM64 profile used
`LUAJIT_MT_ARM64_JIT_EXPERIMENTAL`, `LJ_TRACE_TEST_HELPERS`, and
`LJ_GC2_TEST_HELPERS`; its effective gates were
`ROOT_RECORDER_FAIL_CLOSED=0` and `SIDE_RECORDER_FAIL_CLOSED=1`. The source-only
recorder safepoint contract passed. Logs are in
`/Users/frityet/Scratch/ljmt-recorder-combined-mask-20261029/arm64.log` and
`/Users/frityet/Scratch/ljmt-recorder-combined-mask-20261029/x64.log`.
