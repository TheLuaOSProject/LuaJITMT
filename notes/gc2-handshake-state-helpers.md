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

The global GC2 handshake fields should only be accessed through this helper
surface outside the helper bodies. That ownership rule is documented here and
beside the helpers; `m3_safepoint_handshake`, `m3_vm_safepoint`, and
`m9_gc_stats` cover the observable behavior instead of source-text matching.

Validation:

- `tools/ci/lua_test.sh m3_safepoint_handshake`
- `tools/ci/lua_test.sh m3_vm_safepoint`
- `tools/ci/lua_test.sh m9_gc_stats`
- `git diff --check`
