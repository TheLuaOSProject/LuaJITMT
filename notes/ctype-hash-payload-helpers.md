CType hash payload helpers

- Routed `ctype_hash_findtype()` duplicate-type comparison through
  `ctype_info_acq()` and `ctype_size_acq()` before testing for abandoned
  records and matching `(info, size)`.
- Routed `ctype_hash_findname()` through `ctype_info_acq()` before checking
  abandoned state and type masks.
- Routed `ctype_addtype()` through `ctype_info_acq()`/`ctype_size_acq()` when
  computing the type hash for publication.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_ctype_hash_publish` behavior/counter fixtures and code-adjacent helper docs; raw-field source inventories are not pass/fail contracts.

Verification:

- tools/ci/m7_ffi_ctype_hash_publish.sh
- tools/ci/m7_ffi_ctype_ticket_intern.sh
- tools/ci/m7_ffi_ctype_name_claim.sh
- tools/ci/m7_ffi_cparse_rollback.sh
- git diff --check
