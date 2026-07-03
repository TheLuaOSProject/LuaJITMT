# TG safepoint mirror helper slice

This slice routes TG-local safepoint mirror words through helper accessors:

- `lj_tg_poll_*()` publishes, clears, and reads the per-TG poll word.
- `lj_tg_reqmask_*()` publishes, clears, reads, and consumes the per-TG action
  mask.
- `lj_tg_hs_epoch_ack_*()` publishes, reads, and CAS-claims the acknowledged
  handshake epoch.

Runtime users in `lj_tg.c` and `lj_safepoint.c` now call helper accessors
instead of spelling ad hoc atomics against `TGState.poll`, `reqmask`, or
`hs_epoch_ack`. Test fixtures still inspect these words directly to assert
handshake state transitions.

Production access to the TG-local safepoint mirror must stay behind the helper
surface, with helper bodies as the single raw-access point. `m3_safepoint_handshake`,
`m3_vm_safepoint`, and threading fixture cases own the observable behavior
instead of source-text matching.

Validation:

- `tools/ci/lua_test.sh m3_safepoint_handshake`
- `tools/ci/lua_test.sh m3_vm_safepoint`
- `tools/ci/lua_test.sh m4_thr_substrate`
- `tools/ci/lua_test.sh m4_threading_api`
- `git diff --check`
