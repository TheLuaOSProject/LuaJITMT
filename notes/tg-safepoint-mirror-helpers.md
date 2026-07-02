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

`tools/ci/m3_safepoint_handshake.sh` rejects future raw production access to
the TG-local safepoint mirror while leaving the helper bodies as the single
raw-access point.

Validation:

- `tools/ci/m3_safepoint_handshake.sh`
- `tools/ci/m3_vm_safepoint.sh`
- `tools/ci/m4_thr_substrate.sh`
- `tools/ci/m4_threading_capi.sh`
- `git diff --check`
