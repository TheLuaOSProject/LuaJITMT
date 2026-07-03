# Recorded FFI Library Helper Loads

The remaining recorded FFI library metadata helpers in `lj_crecord.c` now avoid
raw shared `CType.info` / `CType.size` reads.

Converted paths:

- Immediate FFI recorder conversions from known `CTypeID`s now use
  `crec_ct_tv_id()` / `crec_bit64_argid()` to copy the destination ctype into a
  local snapshot before calling the conversion helpers. This removes the
  remaining direct `ctype_get()` table-slot pointers from `lj_crecord.c`.
- Immediate primitive/string source conversions inside `crec_ct_tv()` also use
  the predefined-aware raw-id helper, so Lua string sources record through the
  immutable `const char[]` ctype without taking the parser-busy abort path.
- `recff_ffi_fill()` snapshots destination ctype metadata before pointer
  alignment resolution.
- `recff_ffi_string()` and `recff_ffi_copy()` snapshot their predefined pointer
  destination ctypes before recording pointer/length conversions.
- `recff_ffi_xof()` snapshots the queried ctype metadata before rejecting
  variable-length `ffi.sizeof()`.
- `crec_bit64_type()` snapshots enum child metadata and size before selecting
  signed vs. unsigned 64-bit cdata rank.
- `lj_crecord_tonumber()` snapshots enum child metadata and size before
  selecting int32 vs. double conversion.

Invariant check:

- `tests/t-ffi-layout-snapshot.c` now holds the ctype parse token while the JIT
  records ctype-object `ffi.sizeof(ct)`. The recorder must abort with CTBUSY
  and release the token instead of reading shared ctype metadata through a raw
  table pointer.
- `tests/t-ffi-recorder-libmeta-busy.c` applies the same parser-busy trace
  abort guard to recorded `ffi.fill(cdata, ...)`, `tonumber(cdata)`, and bit64
  cdata argument classification, including enum child metadata snapshots.
  Predefined `void *` / `const void *` / `const char *` traces for
  `ffi.fill()`, `ffi.copy()`, and `ffi.string()` must record without CTBUSY
  under an unrelated parser token.
- Do not replace this with a legacy wrapper. The project policy in
  `notes/ci-invariant-testing.md` requires behavior fixtures or generated
  artifact checks for CI coverage.

Validation:

- `make -C src -j2`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_carith_l`
- `git diff --check`
