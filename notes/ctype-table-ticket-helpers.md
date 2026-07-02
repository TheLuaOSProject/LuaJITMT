CType table header and ticket helpers

- Added `ctype_tabh_rel()`, `ctype_tabh_cas()`, `ctype_top_rel()`, and
  `ctype_top_cas()` alongside the existing acquire helpers.
- Routed ctype table growth publication, initial table/top publication, ticket
  reservation, and ctype ID assertion checks through the helper API.
- Documented the invariant formerly checked by `m7_ffi_ctype_ticket_intern`: raw
  implementation-side `cts->tabh` and `cts->top` access.

Verification:

- tools/ci/m7_ffi_ctype_ticket_intern.sh
- tools/ci/m7_ffi_ctype_tab_retire.sh
- git diff --check
