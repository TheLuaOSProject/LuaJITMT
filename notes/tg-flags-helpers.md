TG flag helper surface
======================

Status: implemented and guarded.

Changes:

- Added `lj_tg_flags_*` helpers for the shared `TGState.tg_flags` byte.
- Routed production STOPREQ, DEAD, arena-internal, and hugetab flag tests
  through acquire helper reads.
- Routed production flag set/clear operations through atomic helper operations.
- Documented the invariant formerly checked by `m3_safepoint_handshake`: raw production
  `tg_flags` access outside `src/lj_tg.h`.

Validation:

- `make -C src -j$(nproc)`
- `tools/ci/m3_safepoint_handshake.sh`
- `tools/ci/m3_gc2_scaffold.sh`
- `tools/ci/m0_matrix.sh`
