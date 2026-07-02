CType table retire epoch helpers

- Added `ctype_tab_retire_epoch_acq()` and `ctype_tab_retire_epoch_rel()` for
  the safepoint epoch attached to retired CTypeTab generations.
- Routed table allocation initialization, table retirement publication, and
  epoch reclaim checks through the helper API.
- Documented the invariant formerly checked by `m7_ffi_ctype_tab_retire`: raw
  implementation-side `retire_epoch` access.

Verification:

- tools/ci/m7_ffi_ctype_tab_retire.sh
- git diff --check
