# Recorder safepoint deferral: combined action coverage

The protected-recorder flush fixture now runs the real attached-peer handshake
with `EXIT_TRACES | FLUSHJ | SCAN_OWNER_ROOTS | FLUSH_SSB`. The leader is
paused after publishing the owner's request. The recorder consumes its abort
and releases the JIT token while the leader remains paused; a later owner
boundary reaches the consumed-poll wait with `reqmask == 0`, the recorder
idle, the token released, and the same combined mask still globally published.
The fixture asserts this observation and completes the real leader handshake.

This exercises the `actions == 0` consumed-poll path after the deferral. It
does not synthesize an epoch or request mask. The active-recorder claim-loser
path is not forced here: making a second same-TG acknowledgement race the
owner's epoch claim would require an artificial concurrent owner ACK. The
existing local-native duplicate test covers claim-loser/duplicate root-scan
behavior without an active recorder token.

Focused validation on macOS arm64: strict `-Wall -Wextra -Werror` fixture
compile and `t-jit-recorder-safepoint` passed with `LJ_TRACE_TEST_HELPERS` and
`LJ_GC2_TEST_HELPERS`. The source-only recorder safepoint contract also passed.
