# GC2 handshake state helper slice

This slice routes the global GC2 soft-handshake control words through
field-specific helper accessors:

- `gc2_hs_epoch_*()` publishes and reads the leader handshake generation.
- `gc2_hs_pending_*()` publishes, increments, decrements, waits on, and wakes
  the outstanding-ack countdown.
- `gc2_hs_actions_*()` publishes and reads the current action mask.
- `gc2_hs_signal_ns_*()` publishes and reads the current handshake timestamp.
- `gc2_hs_ack_latency_*()` publishes, updates, and reads poll-ack latency
  telemetry, including the histogram bucket array.

Runtime users in `lj_safepoint.c`, `lj_tg.c`, `lj_gc2.c`, and
`threading.gcstats()` now call helper accessors instead of spelling ad hoc
atomics or futex operations against those `GC2State` words. A follow-up
TG-local mirror helper slice routes `reqmask`, `poll`, and `hs_epoch_ack`
through `lj_tg_*` safepoint helpers.

`tools/ci/m3_safepoint_handshake.sh` rejects future raw production access to
the global GC2 handshake fields while leaving the helper bodies as the single
raw-access point.

Validation:

- `tools/ci/m3_safepoint_handshake.sh`
- `tools/ci/m3_vm_safepoint.sh`
- `tools/ci/m9_gc_stats.sh`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`
