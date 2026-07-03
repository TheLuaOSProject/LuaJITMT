CType table slot helper

- Added `ctype_tab_slot()` as the single helper for deriving a `CType *` slot
  pointer from an acquired `CTypeTab` generation.
- Routed `lj_ctype.c` snapshot readers, reservation result publication, and FFI
  type/layout snapshot readers through the helper instead of direct
  `CTypeTab.tab[]` indexing.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_ctype_tab_retire` behavior/counter fixtures and code-adjacent helper docs; raw-field source inventories are not pass/fail contracts.

Verification:

- tools/ci/m7_ffi_ctype_tab_retire.sh
- tools/ci/m7_ffi_typeinfo_snapshot.sh
- tools/ci/m7_ffi_ctype_name_claim.sh
- tools/ci/m7_ffi_ctype_ticket_intern.sh
- git diff --check
