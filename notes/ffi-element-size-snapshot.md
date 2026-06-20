# FFI element-size snapshots

## Context

Numeric cdata indexing and pointer arithmetic only need the pointed-to element
size, but the interpreter and recorder paths still acquired the cparser token
to wait out failed `ffi.cdef()` rollback windows. That made stable reads of
already-published pointer element types serialize with `ffi.cdef()`.

## Change

Added `lj_ctype_size_snapshot(cts, id, &sz)`.

- Acquire-loads `CTState.parse_token` and rejects odd parser windows.
- Snapshots the current CType table header and top.
- Bounded-walks attribute chains using acquire loads of `CType.info` and
  `CType.size`.
- Rechecks the same even parser sequence before accepting the size.
- Returns `-1` for active/overlapping parser work or inconsistent table
  snapshots so callers keep their existing locked fallback.

Routed the stable path through this helper in:

- `lj_cdata_index_l()` for numeric pointer/array indexing.
- `carith_ptr()` for interpreted pointer add/sub/diff.
- `recff_cdata_index()` and `crec_arith_ptr()` for recorder-side cdata numeric
  indexing and pointer arithmetic specialization.

The helper intentionally matches `lj_ctype_size()`: it skips attributes, does
not support VLA/VLS, and returns `CTSIZE_INVALID` for incomplete/no-size types.

## Coverage

Added `tests/t-ffi-element-size-snapshot.c`, wired into
`m7_ffi_typeinfo_snapshot`. It verifies stable interpreted and traced numeric
cdata indexing plus pointer arithmetic do not advance the cparser sequence.
The existing rollback reader remains in the same suite and still covers
numeric indexing and pointer arithmetic during failed parser rollback.

## Verification

- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `tools/ci/lua_test.sh m7_ffi_cparse_rollback`
- `tools/ci/lua_test.sh m7_ffi_cdata_get_l`
- `tools/ci/lua_test.sh m7_ffi_carith_l`
- `tools/ci/lua_test.sh m7_ffi_jit_cnew`
