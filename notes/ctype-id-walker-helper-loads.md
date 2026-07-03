CType ID walker helper loads

- Routed `ctype_childid()` through `ctype_info_acq()` for child assertions and
  child ID extraction.
- Routed `ctype_rawid()` and `ctype_rawrefid()` through `ctype_info_acq()` while
  walking attribute/reference chains.
- Routed `ctype_rawchildid()` through helper-backed child extraction and a
  helper-backed attribute check for the child record.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_ctype_pointer_ids` behavior/counter fixtures and code-adjacent helper docs; raw-field source inventories are not pass/fail contracts.

Verification:

- tools/ci/m7_ffi_ctype_pointer_ids.sh
- tools/ci/m7_ffi_typeinfo_snapshot.sh
- tools/ci/m7_ffi_ctype_ticket_intern.sh
- tools/ci/m7_ffi_cparse_rollback.sh
- git diff --check
