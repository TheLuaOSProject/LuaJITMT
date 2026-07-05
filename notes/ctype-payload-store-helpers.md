CType payload store helpers

- Routed core `CType.info`/`CType.size` publication stores through
  `ctype_info_rel()` and `ctype_size_rel()` in `lj_ctype_new_l()`,
  `ctype_intern_l()`, and CTState bootstrap.
- Routed parser rollback abandon payload stores through the same release-store
  helper surface before publishing the abandoned records.
- Routed rollback saved-record copies and duplicate-loser/non-parser abandoned
  records through `ctype_copy_rel()` plus the release-store helper surface before
  publishing or restoring them.
- Routed parser-created shared rows for function types, constants, fields,
  enum constants, parameters, typedefs, externs, and redirected-symbol
  attributes through `ctype_info_rel()`/`ctype_size_rel()` before publication.
- Updated the duplicate-loser assertion to acquire-load the abandoned payload
  through `ctype_info_acq()`.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_ctype_ticket_intern` behavior/counter fixtures; the helper comments carry the ordering rationale.

Verification:

- tools/ci/m7_ffi_ctype_ticket_intern.sh
- tools/ci/m7_ffi_cparse_rollback.sh
- tools/ci/m7_ffi_ctype_name_claim.sh
- tools/ci/m7_ffi_typeinfo_snapshot.sh
- git diff --check
