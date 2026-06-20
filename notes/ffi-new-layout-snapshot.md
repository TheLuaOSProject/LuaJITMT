FFI ffi.new layout snapshot slice
=================================

- Stable non-string `ffi.new(ct, ...)` now snapshots ctype layout without taking
  the parser token. String declarations still use the parser path.
- Added a sequence-checked ctype info/raw-type snapshot helper for recorder
  allocation specialization. `recff_cdata_index()`/CNEW recording now falls back
  to the parser token only when the snapshot overlaps parser mutation.
- The interpreter `ffi.new` path reuses the layout snapshot machinery to compute
  raw ID, info, fixed size, and VLA/VLS size. Invalid/abandoned snapshot results
  report the same invalid-size error path instead of allocating from partial
  data.
- Extended `t-ffi-layout-snapshot.c` so stable struct and VLA `ffi.new(ct, ...)`
  calls must leave `parse_token` unchanged with JIT enabled.
- Coverage: `make -C src`, `m7_ffi_typeinfo_snapshot`, `m7_ffi_jit_cnew`,
  `m7_ffi_cparse_rollback`, and full `m7_ffi`.
