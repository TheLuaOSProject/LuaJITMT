FINREG CTState root-head helper slice

- Added typed helpers for `CTState.fin_head`, `fin_order_head`, and
  `fin_order_retired` acquire loads, CAS publication/splice, release publish,
  and teardown exchange operations.
- Routed FINREG generation scans, new-generation CAS publish, ordered
  registration publish, ordered retire, marking, close-time discovery, pending
  checks, and teardown through those helpers.
- Extended `tools/ci/m7_ffi_finreg.sh` to reject raw FINREG CTState root
  access in implementation files.
- Follow-up: `tests/t-gc2-traverse.c` now uses `fin_order_head_acq()` for its
  ordered cdata FINREG reference counter, and the same guard rejects raw
  `CTState.fin_*` root access in that fixture.

Verification:

- tools/ci/m7_ffi_finreg.sh
- tools/ci/m3_gc2_scaffold.sh
- tools/ci/m3_gc2_paranoia.sh
- tools/ci/m8_weak.sh
- tools/ci/m0_source_guard.sh
- git diff --check
