CType table capacity helpers

- Added `ctype_tab_sizetab_acq()` and `ctype_tab_sizetab_rel()` for the
  published CTypeTab capacity.
- Routed table allocation, free sizing, growth sizing, snapshot bounds checks,
  FFI type/layout snapshot bounds checks, and table-retire test assertions
  through the helper API.
- Extended `tools/ci/m7_ffi_ctype_tab_retire.sh` to reject raw
  implementation-side `sizetab` access.

Verification:

- tools/ci/m7_ffi_ctype_tab_retire.sh
- tools/ci/m7_ffi_ctype_ticket_intern.sh
- tools/ci/m7_ffi_typeinfo_snapshot.sh
- tools/ci/m0_source_guard.sh
- git diff --check
