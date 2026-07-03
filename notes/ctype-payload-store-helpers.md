CType payload store helpers

- Routed core `CType.info`/`CType.size` publication stores through
  `ctype_info_rel()` and `ctype_size_rel()` in `lj_ctype_new_l()`,
  `ctype_intern_l()`, and CTState bootstrap.
- Routed parser rollback abandon payload stores through the same release-store
  helper surface before publishing the abandoned records.
- Updated the duplicate-loser assertion to acquire-load the abandoned payload
  through `ctype_info_acq()`.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_ctype_ticket_intern` behavior/counter fixtures; the helper comments carry the ordering rationale.

Verification:

- tools/ci/m7_ffi_ctype_ticket_intern.sh
- tools/ci/m7_ffi_cparse_rollback.sh
- tools/ci/m7_ffi_ctype_name_claim.sh
- tools/ci/m7_ffi_typeinfo_snapshot.sh
- git diff --check
