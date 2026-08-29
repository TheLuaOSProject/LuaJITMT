FINREG generation liveness helper slice

- Added `fin_gen_tab_enable_rel()` and `fin_gen_tab_enabled_acq()` next to the
  existing FINREG generation table helpers.
- Routed FINREG table construction, lookup visibility, new-generation
  disabled-head checks, FINREG table classification, and ordered
  P_WEAK/close-time/pending discovery through the helper API.
- Documented why FINREG generation liveness is owned by the helper surface:
  disabled-generation sentinels and active generation tables both flow through
  metatable slots, so lookup and close-time scans need one acquire/release
  vocabulary. The runnable coverage stays in FINREG and weak/finalizer
  behavior fixtures; helper comments carry the implementation rationale.

Verification:

- tools/ci/m7_ffi_finreg.sh
- tools/ci/m8_weak.sh
- git diff --check
