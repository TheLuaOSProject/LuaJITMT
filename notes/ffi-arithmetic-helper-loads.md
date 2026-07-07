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
  avoiding a live ctype-table pointer in the n-ary cdata bit-op loop. A later
  cleanup made that copy use `lj_ctype_info_predefined()` directly because
  `lj_carith_check64()` only selects predefined `int64_t`/`uint64_t` result
  IDs.
- Follow-up cdata arithmetic lifetime cleanup routes `carith_set_operand_id()`,
  pointer-index conversion, int64 result conversion, enum-source handoff, and
  `lj_carith_check64()` destination conversion through a local exact-record
  snapshot/wait helper. Parser-created IDs no longer reopen the ctype table
  with a raw pointer after intern/snapshot work; active recorder paths still
  abort with `CTBUSY` instead of waiting.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_carith_l` behavior/counter fixtures; the helper comments carry the ordering rationale.
- Extended `tests/suites/m7_ffi.lua` to document raw arithmetic `ctype_get(cts,
  ...)` live-pointer reuse outside immediate local CType copies in
  `src/lj_carith.c` and `src/lib_bit.c`.

Verification:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m7_ffi_carith_l`
- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_carith_l
- LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot m7_ffi_cparse_rollback
- git diff --check
