ffi.cast destination type snapshot
==================================

Stable non-string `ffi.cast(ct, value)` now snapshots the destination ctype
metadata before conversion. The fast path uses `lj_ctype_info_snapshot()` to
copy the raw type record, type info, size, and raw ID into a local `CType`.

String declarations still use the parser-backed path, preserving declaration
parsing behavior. If the stable ctype-object snapshot overlaps an active parser
mutation, `ffi.cast` falls back under the parser token and then converts using
the locked raw ctype record.

Coverage added:

- stable enum ctype-object casts now cover numeric enum casts as well as cdata
  enum casts;
- cparser rollback reader now races failed enum cdefs against numeric enum
  casts and checks the failed declaration does not leave a usable rolled-back
  layout behind.

Validation:

- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `tools/ci/lua_test.sh m7_ffi_cparse_rollback`
- `tools/ci/lua_test.sh m7_ffi_carith_l`
- `tools/ci/lua_test.sh m7_ffi`
