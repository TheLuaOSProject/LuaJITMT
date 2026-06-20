FINREG generation liveness helper slice

- Added `fin_gen_tab_enable_rel()` and `fin_gen_tab_enabled_acq()` next to the
  existing FINREG generation table helpers.
- Routed FINREG table construction, lookup visibility, new-generation
  disabled-head checks, FINREG table classification, and ordered
  P_WEAK/close-time/pending discovery through the helper API.
- Extended `tools/ci/m7_ffi_finreg.sh` to reject raw FINREG generation
  liveness access through implementation-side `t->metatable`, `ft->metatable`,
  or `headtab->metatable` operations.

Verification:

- tools/ci/m7_ffi_finreg.sh
- tools/ci/m8_weak.sh
- tools/ci/m0_source_guard.sh
- git diff --check
