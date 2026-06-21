CType table slot helper

- Added `ctype_tab_slot()` as the single helper for deriving a `CType *` slot
  pointer from an acquired `CTypeTab` generation.
- Routed `lj_ctype.c` snapshot readers, reservation result publication, and FFI
  type/layout snapshot readers through the helper instead of direct
  `CTypeTab.tab[]` indexing.
- Extended `tools/ci/m7_ffi_ctype_tab_retire.sh` to reject raw implementation
  indexing through `CTypeTab.tab[]` in the guarded ctype/FFI implementation
  files.

Verification:

- tools/ci/m7_ffi_ctype_tab_retire.sh
- tools/ci/m7_ffi_typeinfo_snapshot.sh
- tools/ci/m7_ffi_ctype_name_claim.sh
- tools/ci/m7_ffi_ctype_ticket_intern.sh
- tools/ci/m0_source_guard.sh
- git diff --check
