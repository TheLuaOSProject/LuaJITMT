FINREG CTState root-head helper slice

- Added typed helpers for `CTState.fin_head`, `fin_order_head`, and
  `fin_order_retired` acquire loads, CAS publication/splice, release publish,
  and teardown exchange operations.
- Routed FINREG generation scans, new-generation CAS publish, ordered
  registration publish, ordered retire, marking, close-time discovery, pending
  checks, and teardown through those helpers.
- Extended `tools/ci/m7_ffi_finreg.sh` to reject raw FINREG CTState root
  access in implementation files.

Verification:

- tools/ci/m7_ffi_finreg.sh
- tools/ci/m8_weak.sh
- tools/ci/m0_source_guard.sh
- git diff --check
