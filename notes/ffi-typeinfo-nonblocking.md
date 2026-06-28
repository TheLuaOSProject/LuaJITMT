# FFI typeinfo nonblocking snapshot

`ffi.typeinfo()` is an internal read-only API. It now uses a direct immutable
predefined-ID snapshot before the existing sequence-checked
`lj_ctype_snapshot()` path. Parser-created IDs still return `nil` if the
parser token is odd or a table/top snapshot races a mutation, instead of
waiting on `lj_ctype_parse_lock()`.

This keeps mutable `ffi.cdef()` serialized while removing a parser-lock fallback
from a non-mutating query. Stable completed CType IDs still return the same
metadata table, and incomplete stable structs still return a table without a
`size` field. Predefined CType IDs are installed during VM initialization and
are immutable, so their metadata can be returned even while an unrelated
`ffi.cdef()` transaction is active.

The shared `lj_ctype_info_wait()` helper now uses the same predefined snapshot
before parking. This keeps fallback readers such as cdata arithmetic error and
metamethod paths from waiting on unrelated parser work just to inspect an
immutable scalar CType.

Coverage:

- `tests/t-ffi-typeinfo-snapshot.c` manually holds `CTState.parse_token` odd,
  verifies `ffi.typeinfo(int_id)` still returns predefined metadata, verifies a
  parser-created `ffi.typeinfo(valid_id)` returns `nil` without acquiring the
  parser token, then verifies the same parser-created ID returns normal metadata
  after release.
- `tests/t-ffi-metatv-snapshot.c` holds the parser token while a predefined
  `int` cdata arithmetic miss flows through type-info and ctype-metamethod
  fallback without waiting.
- `tests/t-ffi-cparse-rollback-reader.lua` accepts transient `nil` during active
  parser rollback windows while preserving final incomplete-type checks.

Verification:

- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `tools/ci/lua_test.sh m7_ffi_cparse_rollback`
- `tools/ci/m7_ffi_blocking.sh`
