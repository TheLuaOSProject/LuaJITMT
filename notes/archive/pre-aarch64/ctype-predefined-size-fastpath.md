# Predefined ctype size reads

`lj_ctype_size_wait()` now resolves immutable predefined CType IDs without
waiting for an unrelated active parser token. This covers interpreter cdata
numeric indexing and pointer arithmetic when the element type is a predefined
record such as `int`.

The fast path is deliberately narrow. It only follows attribute chains that
stay inside the predefined range and falls back to the existing
sequence-checked wait/retry helper for parser-created typedefs, user-defined
records, incomplete records, and rollback-sensitive cases.

`tests/t-ffi-element-size-snapshot.c` holds the parser token while exercising
predefined `int *` indexing, pointer addition, and pointer difference. The same
fixture keeps the typedef-backed pointer cases on the native wait path.

Validation target:
- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
