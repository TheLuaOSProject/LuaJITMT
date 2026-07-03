TG flag helper surface
======================

Status: implemented and guarded.

Changes:

- Added `lj_tg_flags_*` helpers for the shared `TGState.tg_flags` byte.
- Routed production STOPREQ, DEAD, arena-internal, and hugetab flag tests
  through acquire helper reads.
- Routed production flag set/clear operations through atomic helper operations.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m3_safepoint_handshake` behavior/counter fixtures and code-adjacent helper docs; raw-field source inventories are not pass/fail contracts.

Validation:

- `make -C src -j$(nproc)`
- `tools/ci/m3_safepoint_handshake.sh`
- `tools/ci/m3_gc2_scaffold.sh`
- `tools/ci/m0_matrix.sh`
