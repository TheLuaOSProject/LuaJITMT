CType payload store helpers

- Routed core `CType.info`/`CType.size` publication stores through
  `ctype_info_rel()` and `ctype_size_rel()` in `lj_ctype_new_l()`,
  `ctype_intern_l()`, and CTState bootstrap.
- Routed parser rollback abandon payload stores through the same release-store
  helper surface before publishing the abandoned records.
- Updated the duplicate-loser assertion to acquire-load the abandoned payload
  through `ctype_info_acq()`.
- Extended `tools/ci/m7_ffi_ctype_ticket_intern.sh` to reject raw core ctype
  payload stores and raw parser `CTA_BAD` abandon payload stores.

Verification:

- tools/ci/m7_ffi_ctype_ticket_intern.sh
- tools/ci/m7_ffi_cparse_rollback.sh
- tools/ci/m7_ffi_ctype_name_claim.sh
- tools/ci/m7_ffi_typeinfo_snapshot.sh
- tools/ci/m0_source_guard.sh
- git diff --check
