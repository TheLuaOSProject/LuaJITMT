ffi.istype fallback helper loads

- Routed the parser-lock `ffi_istype_raw()` comparator through
  `ctype_info_acq()` and `ctype_size_acq()` for its type, size, numeric/void,
  pointer, and struct-pointer branch decisions.
- Kept the existing raw/ref-stripped ID comparisons and `lj_cconv_compatptr()`
  call shape unchanged.
- Documented the invariant formerly checked by `m7_ffi_typeinfo_snapshot`: raw
  `CType.info`/`CType.size` reads inside `ffi_istype_raw()`.

Verification:

- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `tools/ci/m7_ffi_ctype_pointer_ids.sh`
- `git diff --check`
