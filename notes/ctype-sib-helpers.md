CType sibling link helpers

- Added `ctype_sib_acq()` and `ctype_sib_rel()` for the `CType.sib` sibling
  link, matching the existing helper surface for `CType.next`.
- Routed ctype snapshots, FFI typeinfo/typecmp/layout snapshots, name/field
  walks, VLA field walks, parser sibling publication, redirected-symbol
  sibling insertion, and parser rollback abandon clears through the helper API.
- Extended `tools/ci/m7_ffi_typeinfo_snapshot.sh` to reject raw shared
  `CType.sib` acquire-loads and direct shared `ct->sib`-style access in the
  guarded ctype/FFI/parser implementation files.

Verification:

- tools/ci/m7_ffi_typeinfo_snapshot.sh
- tools/ci/m7_ffi_cparse_rollback.sh
- tools/ci/m7_ffi_ctype_name_claim.sh
- tools/ci/m7_ffi_ctype_ticket_intern.sh
- tools/ci/m0_source_guard.sh
- git diff --check
