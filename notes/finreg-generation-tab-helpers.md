FINREG generation table helper slice

- Added `fin_gen_tab_acq()` and `fin_gen_tab_rel()` for the
  `FinRegGen.tab` publication edge.
- Routed FINREG generation readers in ctype lookup, claim scans, marking, and
  close-time disable through the helpers.
- Added `fin_gen_tab_disable_rel()` for the release-store generation disable
  path used by `lj_gc_finalize_cdata_disable()`.
- Extended `tools/ci/m7_ffi_finreg.sh` to reject raw generation table access
  in FINREG implementation code.

Verification:

- tools/ci/m7_ffi_finreg.sh
- tools/ci/m8_weak.sh
- tools/ci/m0_source_guard.sh
- git diff --check
