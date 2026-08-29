FINREG CTState root-head helper slice

- Added typed helpers for `CTState.fin_head`, `fin_order_head`, and
  `fin_order_retired` acquire loads, CAS publication/splice, release publish,
  and teardown exchange operations.
- Routed FINREG generation scans, new-generation CAS publish, ordered
  registration publish, ordered retire, marking, close-time discovery, pending
  checks, and teardown through those helpers.
- Documented why FINREG CTState roots use the helper layer: generation publish,
  ordered registration, marking, teardown, and close-time discovery share the
  same acquire/release boundary.
- Follow-up: `tests/t-gc2-traverse.c` now uses `fin_order_head_acq()` for its
  ordered cdata FINREG reference counter.

Verification:

- tools/ci/m7_ffi_finreg.sh
- tools/ci/m3_gc2_scaffold.sh
- tools/ci/m3_gc2_paranoia.sh
- tools/ci/m8_weak.sh
- git diff --check
