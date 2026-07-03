CType sibling link helpers

- Added `ctype_sib_acq()` and `ctype_sib_rel()` for the `CType.sib` sibling
  link, matching the existing helper surface for `CType.next`.
- Routed ctype snapshots, FFI typeinfo/typecmp/layout snapshots, name/field
  walks, VLA field walks, parser sibling publication, redirected-symbol
  sibling insertion, and parser rollback abandon clears through the helper API.
- Documented why shared `CType.sib` links use acquire/release helpers: parser
  rollback, redirected symbols, and recorder snapshots can race with readers.

Verification:

- tools/ci/m7_ffi_typeinfo_snapshot.sh
- tools/ci/m7_ffi_cparse_rollback.sh
- tools/ci/m7_ffi_ctype_name_claim.sh
- tools/ci/m7_ffi_ctype_ticket_intern.sh
- git diff --check
