FFI parse token helper surface

- Added `ctype_parse_token_acq()`, `ctype_parse_token_rel()`,
  `ctype_parse_token_cas()`, `ctype_parse_token_wait()`, and
  `ctype_parse_token_wake()` for the accepted cparse mutation-token bridge.
- Routed `lj_ctype_parse_lock()`, `lj_ctype_parse_unlock()`, ctype snapshot
  readers, and FFI type/layout snapshot readers through the helper API.
- Extended `tools/ci/m7_ffi_cdef_token.sh` to reject raw implementation-side
  `cts->parse_token` access.

Verification:

- tools/ci/m7_ffi_cdef_token.sh
- tools/ci/m7_ffi_cparse_rollback.sh
- tools/ci/m7_ffi_typeinfo_snapshot.sh
- tools/ci/m0_source_guard.sh
- git diff --check
