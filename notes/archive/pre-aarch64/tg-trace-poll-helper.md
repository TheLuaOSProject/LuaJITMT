TG trace poll helper surface
============================

Status: implemented and guarded.

Changes:

- Routed the JIT trace-side safepoint-pending check through `lj_tg_poll_acq()`.
- Expanded `tools/ci/m3_safepoint_handshake.sh` to document raw production
  C-side access to `TGState.poll`, `TGState.reqmask`, and
  `TGState.hs_epoch_ack` outside `src/lj_tg.h`.

Validation:

- `make -C src -j$(nproc)`
- `tools/ci/m3_safepoint_handshake.sh`
- `tools/ci/m3_vm_safepoint.sh`
- `tools/ci/m6_jit_flush_hs.sh`
