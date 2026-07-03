CType table header and ticket helpers

- Added `ctype_tabh_rel()`, `ctype_tabh_cas()`, `ctype_top_rel()`, and
  `ctype_top_cas()` alongside the existing acquire helpers.
- Routed ctype table growth publication, initial table/top publication, ticket
  reservation, and ctype ID assertion checks through the helper API.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_ctype_ticket_intern` behavior/counter fixtures and code-adjacent helper docs; raw-field implementation-text inventories are not pass/fail contracts.

Verification:

- tools/ci/m7_ffi_ctype_ticket_intern.sh
- tools/ci/m7_ffi_ctype_tab_retire.sh
- git diff --check
