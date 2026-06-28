# Recorded FFI Library Helper Loads

The remaining recorded FFI library metadata helpers in `lj_crecord.c` now avoid
raw shared `CType.info` / `CType.size` reads.

Converted paths:

- `recff_ffi_fill()` snapshots destination ctype metadata before pointer
  alignment resolution.
- `recff_ffi_xof()` snapshots the queried ctype metadata before rejecting
  variable-length `ffi.sizeof()`.
- `crec_bit64_type()` snapshots enum child metadata and size before selecting
  signed vs. unsigned 64-bit cdata rank.
- `lj_crecord_tonumber()` snapshots enum child metadata and size before
  selecting int32 vs. double conversion.

Guardrail:

- `tests/t-ffi-layout-snapshot.c` now holds the ctype parse token while the JIT
  records ctype-object `ffi.sizeof(ct)`. The recorder must abort with CTBUSY
  and release the token instead of reading shared ctype metadata through a raw
  table pointer.
- `tests/t-ffi-recorder-libmeta-busy.c` applies the same parser-busy trace
  abort guard to recorded `ffi.fill(cdata, ...)`, `tonumber(cdata)`, and bit64
  cdata argument classification, including enum child metadata snapshots.
- Do not replace this with a source-search guard. The project policy in
  `notes/ci-source-search-policy.md` requires behavior fixtures or generated
  artifact checks for CI coverage.

Validation:

- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `tools/ci/lua_test.sh m7_ffi`
- `git diff --check`
