# Recorder Cdata Index Helper Loads

`recff_cdata_index()` now avoids direct shared `CType` table walks while
recording cdata indexing and field access. Raw/root type IDs, reference
children, field result types, cdata integer index types, pointer-to-struct
auto-deref targets, and bitfield store conversion types are copied into local
snapshots before recorder planning continues.

The recorder keeps the same predefined-element compatibility path used by
FFI arithmetic: a shallow pointer ctype whose child is predefined can be copied
without observing an unrelated active parser token. This preserves traced
`int *` indexing and pointer arithmetic behavior while parser-created struct
paths still abort with `CTBUSY` instead of waiting inside the recorder.

The helper snapshots ctype metadata through `ctype_info_acq()` /
`ctype_size_acq()` for:

- pointer and reference base resolution;
- numeric index element sizing and complex-index masking;
- cdata integer index width/sign handling;
- string field lookup fallback metadata and constant fields;
- pointer-to-struct auto-deref decisions;
- reference-field and attribute stripping before delegating to load/store
  conversion helpers.

Coverage:

- `tests/t-ffi-recorder-libmeta-busy.c` now includes traced pointer-to-struct
  field access, cdata-integer pointer indexing, and bitfield assignment while
  the parser token is held, plus hot-loop coverage after abort.
- `tests/t-ffi-element-size-snapshot.c` continues to guard the predefined
  `int *` no-busy trace path.

Guardrail:

- `tools/ci/lua_test.sh m7_ffi_cdata_get_l` covers behavior for cdata reads.

Validation:

- `make -C src -j2`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_cdata_get_l`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_cdata_set_l`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_jit_cnew`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_carith_l`
- `git diff --check`
