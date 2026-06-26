TG GC mirror helper surface
===========================

Status: implemented and guarded for C-side production users.

Changes:

- Added `lj_tg_mark_active_*` and `lj_tg_alloc_black_*` helpers around the
  per-TG GC phase mirrors.
- Preserved acquire/release ordering for C-side mirror publication and barrier
  decisions.
- Routed safepoint phase application, attach catch-up phase adoption, C-side GC2
  barrier predicates, and table bulk-store barrier checks through helpers.
- Extended `tools/ci/m3_safepoint_handshake.sh` to reject raw C-side
  production access to `TGState.mark_active` and `TGState.alloc.alloc_black`
  outside `src/lj_tg.h`.

Note:

- The x64 VM and assembler still intentionally reference these TG offsets for
  generated barrier checks; this slice only guards C-side access.

Validation:

- `make -C src -j$(nproc)`
- `tools/ci/m3_safepoint_handshake.sh`
- `tools/ci/m3_vm_safepoint.sh`
- `tools/ci/m2_arena_gcphase.sh`
- `tools/ci/m5_tab_cas_store.sh`
- `tools/ci/m5_tab_value_publish.sh`
- `tools/ci/m6_jit_barrier_xpoll.sh`
