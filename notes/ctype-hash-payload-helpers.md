CType hash payload helpers

- Routed `ctype_hash_findtype()` duplicate-type comparison through
  `ctype_info_acq()` and `ctype_size_acq()` before testing for abandoned
  records and matching `(info, size)`.
- Routed `ctype_hash_findname()` through `ctype_info_acq()` before checking
  abandoned state and type masks.
- Routed `ctype_addtype()` through `ctype_info_acq()`/`ctype_size_acq()` when
  computing the type hash for publication.
- Extended `tools/ci/m7_ffi_ctype_hash_publish.sh` to reject raw
  `CType.info`/`CType.size` reads in these hash-walker patterns.

Verification:

- tools/ci/m7_ffi_ctype_hash_publish.sh
- tools/ci/m7_ffi_ctype_ticket_intern.sh
- tools/ci/m7_ffi_ctype_name_claim.sh
- tools/ci/m7_ffi_cparse_rollback.sh
- tools/ci/m0_source_guard.sh
- git diff --check
