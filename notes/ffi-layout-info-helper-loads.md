# FFI Layout Info Helper Loads

`ffi_layout_info()` and `ffi_layout_info_raw()` now use helper-backed local
`CType.info` / `CType.size` snapshots while accumulating layout attributes and
returning size information for `ffi.sizeof()`, `ffi.alignof()`, and
`ffi.offsetof()`.

Guardrail:

- `tools/ci/m7_ffi_typeinfo_snapshot.sh` rejects direct local `ct.info` /
  `ct.size` reads in these layout info helpers.

Validation:

- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `git diff --check`
