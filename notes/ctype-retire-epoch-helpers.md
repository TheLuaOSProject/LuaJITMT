CType table retire epoch helpers

- Added `ctype_tab_retire_epoch_acq()` and `ctype_tab_retire_epoch_rel()` for
  the safepoint epoch attached to retired CTypeTab generations.
- Routed table allocation initialization, table retirement publication, and
  epoch reclaim checks through the helper API.
- Extended `tools/ci/m7_ffi_ctype_tab_retire.sh` to reject raw
  implementation-side `retire_epoch` access.

Verification:

- tools/ci/m7_ffi_ctype_tab_retire.sh
- tools/ci/m0_source_guard.sh
- git diff --check
