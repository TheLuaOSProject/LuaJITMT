FINREG generation table helper slice

- Added `fin_gen_tab_acq()` and `fin_gen_tab_rel()` for the
  `FinRegGen.tab` publication edge.
- Routed FINREG generation readers in ctype lookup, claim scans, marking, and
  close-time disable through the helpers.
- Added `fin_gen_tab_disable_rel()` for the release-store generation disable
  path used by close-time GC2 FINREG disable.
- Documented why FINREG generation table access is owned by the helper surface:
  close-time disable and lookup visibility share a publication edge, so readers
  must use a single acquire path. The runnable coverage stays in FINREG and
  weak/finalizer behavior fixtures, not in implementation-text inventories.

Verification:

- tools/ci/m7_ffi_finreg.sh
- tools/ci/m8_weak.sh
- git diff --check
