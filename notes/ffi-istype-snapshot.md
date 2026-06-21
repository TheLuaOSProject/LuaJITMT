ffi.istype comparison snapshot
==============================

Stable `ffi.istype(ct, value)` calls now compare ctype-object arguments from a
sequence-checked CType snapshot instead of directly walking the shared ctype
table. The snapshot path covers raw/ref resolution, attribute chains,
qualifier collection, pointer child compatibility, numeric/void equality, and
the struct-vs-pointer special case.

String declarations still use the parser-backed path. If the stable ctype
snapshot overlaps an active parser mutation, `ffi.istype` falls back under the
parser token and uses the previous raw comparator. That fallback now snapshots
its branch-critical `CType.info`/`CType.size` values through
`ctype_info_acq()`/`ctype_size_acq()` instead of reading shared payload fields
directly.

Coverage:

- added `tests/t-ffi-istype-snapshot.c` for stable scalar, ctype-object,
  struct, pointer, and struct-pointer comparisons that must not advance
  `CTState.parse_token`;
- kept a string declaration case in the same fixture to assert the parser path
  still advances the sequence;
- wired the fixture into `m7_ffi_typeinfo_snapshot`.
- extended `tools/ci/m7_ffi_typeinfo_snapshot.sh` to reject raw
  `CType.info`/`CType.size` reads inside `ffi_istype_raw()`.

Validation:

- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- stock `tests/stock/test/lib/ffi/istype.lua`
- `tools/ci/lua_test.sh m7_ffi_cparse_rollback`
- `tools/ci/lua_test.sh m7_ffi`
