CType ID walker helper loads

- Routed `ctype_childid()` through `ctype_info_acq()` for child assertions and
  child ID extraction.
- Routed `ctype_rawid()` and `ctype_rawrefid()` through `ctype_info_acq()` while
  walking attribute/reference chains.
- Routed `ctype_rawchildid()` through helper-backed child extraction and a
  helper-backed attribute check for the child record.
- Extended `tools/ci/m7_ffi_ctype_pointer_ids.sh` to reject raw `CType.info`
  reads inside the inline ctype ID walker bodies.

Verification:

- tools/ci/m7_ffi_ctype_pointer_ids.sh
- tools/ci/m7_ffi_typeinfo_snapshot.sh
- tools/ci/m7_ffi_ctype_ticket_intern.sh
- tools/ci/m7_ffi_cparse_rollback.sh
- tools/ci/m0_source_guard.sh
- git diff --check
