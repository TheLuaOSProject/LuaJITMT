# FFI element-size snapshots

## Context

Numeric cdata indexing and pointer arithmetic only need the pointed-to element
size, but the interpreter and recorder paths still acquired the cparser token
to wait out failed `ffi.cdef()` rollback windows. That made stable reads of
already-published pointer element types serialize with `ffi.cdef()`.

## Change

Added `lj_ctype_size_snapshot(cts, id, &sz)` and
`lj_ctype_size_wait(L, cts, id, &sz)`.

- Acquire-loads `CTState.parse_token` and rejects odd parser windows.
- Snapshots the current CType table header and top.
- Bounded-walks attribute chains using acquire loads of `CType.info` and
  `CType.size`.
- Rechecks the same even parser sequence before accepting the size.
- Returns `-1` for active/overlapping parser work or inconsistent table
  snapshots so stable readers can park in native time and retry without
  acquiring the parser token.

`lj_ctype_size_wait()` retries by `CTypeID` and only stabilizes the scalar
size result. Callers that need a `CType *` after the wait must refetch it from
the current table.

Predefined immutable element types now bypass the parser wait entirely when
the size walk stays inside the predefined range. This covers interpreter and
recorder numeric indexing and pointer arithmetic for `int *` and similar
predefined element types. Parser-created typedefs, structs, incomplete records,
and other rollback-sensitive records keep the existing sequence-checked native
wait or recorder-abort path.

Routed the stable path through this helper in:

- `lj_cdata_index_l()` for numeric pointer/array indexing.
- `carith_ptr()` for interpreted pointer add/sub/diff.
- `recff_cdata_index()` and `crec_arith_ptr()` for recorder-side cdata numeric
  indexing and pointer arithmetic specialization.

Follow-up cleanup removed the parser-lock fallback from the interpreted
numeric cdata indexing and pointer arithmetic readers. `lj_cdata_index_l()`
uses a shallow acquire snapshot for the current cdata container, so
already-published `int *`-style records keep the predefined no-wait path even
while the parser token is held. Deep child walks still go through the
sequence-checked field/typeinfo/size helpers, and `carith_ptr()` refreshes
cached `CDArith` `CType *` values before falling through to
metamethod/error handling after a wait.

The helper intentionally matches `lj_ctype_size()`: it skips attributes, does
not support VLA/VLS, and returns `CTSIZE_INVALID` for incomplete/no-size types.

## Coverage

Added `tests/t-ffi-element-size-snapshot.c`, wired into
`m7_ffi_typeinfo_snapshot`. It verifies stable interpreted and traced numeric
cdata indexing plus pointer arithmetic do not advance the cparser sequence.
Follow-up coverage now holds the parser token, runs predefined `int *` numeric
cdata indexing, pointer addition, and pointer difference without parking. It
also holds the token during trace recording of the same predefined operations
and verifies no parser-busy trace abort is reported. Parser-created struct
pointer operations still park in native wait until release or abort recording
instead of waiting from the recorder. This turns the old no-lock source-shape
expectation into behavior.
The existing rollback reader remains in the same suite and still covers
numeric indexing and pointer arithmetic during failed parser rollback.

## Verification

- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `tools/ci/lua_test.sh m7_ffi_cparse_rollback`
- `tools/ci/lua_test.sh m7_ffi_cdata_get_l`
- `tools/ci/lua_test.sh m7_ffi_carith_l`
- `tools/ci/lua_test.sh m7_ffi_jit_cnew`
- `tools/ci/lua_test.sh m7_ffi_carith_l m7_ffi_cdata_get_l m7_ffi_cparse_rollback`
