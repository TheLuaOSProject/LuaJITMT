# Recorded Cdata Arithmetic Helper Loads

`recff_cdata_arith()` now keeps operand ctype metadata in recorder-local
`CType` snapshots while normalizing operands before dispatching to the int64,
pointer, or metamethod recorder helpers.

The helper-backed snapshots cover cdata raw type resolution, pointer/reference
resolution, enum child resolution, function cdata normalization to pointers,
numeric payload loads, string-to-enum lookup fallback, and string-to-pointer
decay. Function-pointer interning no longer refreshes prior operands through
live ctype table pointers; previously normalized operands remain stack copies.
The matched enum-constant type is also copied through the same helper path
before recording the string guard.

Coverage:

- `tests/t-ffi-recorder-libmeta-busy.c` records parser-created enum arithmetic,
  enum-string comparison, and parser-created struct pointer arithmetic while
  the parser token is held, and verifies the recorder aborts instead of waiting.
- The same fixture keeps a hot-loop path after abort for enum arithmetic,
  enum-string equality, and pointer add/diff.

Invariant check:

- `tools/ci/lua_test.sh m7_ffi_carith_l` covers arithmetic behavior and the
  interpreter-side ctype snapshot fixtures.

Validation:

- `make -C src -j2`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_carith_l`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_jit_cnew`
- `git diff --check`
