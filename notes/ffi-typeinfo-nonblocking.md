# FFI typeinfo nonblocking snapshot

`ffi.typeinfo()` is an internal read-only API. It now uses only the existing
sequence-checked `lj_ctype_snapshot()` path and returns `nil` if the parser
token is odd or a table/top snapshot races a mutation, instead of waiting on
`lj_ctype_parse_lock()`.

This keeps mutable `ffi.cdef()` serialized while removing a parser-lock fallback
from a non-mutating query. Stable completed CType IDs still return the same
metadata table, and incomplete stable structs still return a table without a
`size` field.

Coverage:

- `tests/t-ffi-typeinfo-snapshot.c` manually holds `CTState.parse_token` odd and
  verifies `ffi.typeinfo(valid_id)` returns `nil` without acquiring the parser
  token, then verifies the same ID returns normal metadata after release.
- `tests/t-ffi-cparse-rollback-reader.lua` accepts transient `nil` during active
  parser rollback windows while preserving final incomplete-type checks.

Verification:

- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `tools/ci/lua_test.sh m7_ffi_cparse_rollback`
- `tools/ci/m7_ffi_blocking.sh`
