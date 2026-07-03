CType table capacity helpers

- Added `ctype_tab_sizetab_acq()` and `ctype_tab_sizetab_rel()` for the
  published CTypeTab capacity.
- Routed table allocation, free sizing, growth sizing, snapshot bounds checks,
  FFI type/layout snapshot bounds checks, and table-retire test assertions
  through the helper API.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_ctype_tab_retire` behavior/counter fixtures and code-adjacent helper docs; raw-field implementation-text inventories are not pass/fail contracts.

Verification:

- tools/ci/m7_ffi_ctype_tab_retire.sh
- tools/ci/m7_ffi_ctype_ticket_intern.sh
- tools/ci/m7_ffi_typeinfo_snapshot.sh
- git diff --check
