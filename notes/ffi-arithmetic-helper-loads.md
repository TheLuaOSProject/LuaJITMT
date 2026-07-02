FFI arithmetic helper loads

- Routed `carith_checkarg()` through `ctype_info_acq()` and `ctype_size_acq()`
  while normalizing cdata, pointer/reference/function, enum, and enum-string
  operands.
- Routed `carith_ptr()` through helper-backed payload reads for pointer/refarray
  classification, pointer differences, pointer-plus-index allocation, and
  swapped pointer/index operands.
- Routed `carith_int64()`, `lj_carith_meta()`, and `lj_carith_check64()`
  through helper-backed payload reads for numeric rank selection, metamethod
  lookup/error classification, and bit-library cdata operands.
- Follow-up operand lifetime cleanup stores arithmetic operands in `CDArith`
  stack snapshots, confines `ctype_get(cts, ...)` to immediate local copies,
  removes the wait-afterward refresh helpers, and returns `lj_carith_check64()`
  source ctypes through caller-owned snapshots instead of live ctype-table
  pointers.
- Follow-up bit-library cleanup snapshots the selected 64-bit result CType for
  `bit.band`/`bit.bor`/`bit.bxor` before converting each TValue operand,
  avoiding a live `ctype_get(cts, id)` table pointer in the n-ary cdata bit-op
  loop.
- Documented the invariant formerly checked by `m7_ffi_carith_l`: raw `CType.info` and
  `CType.size` reads in `src/lj_carith.c`.
- Extended `tests/suites/m7_ffi.lua` to document raw arithmetic `ctype_get(cts,
  ...)` live-pointer reuse outside immediate local CType copies in
  `src/lj_carith.c` and `src/lib_bit.c`.

Verification:

- LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_carith_l
- LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot m7_ffi_cparse_rollback
- git diff --check
