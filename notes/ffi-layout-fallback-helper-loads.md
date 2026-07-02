FFI layout fallback helper loads

- Routed the parser-lock `ffi.sizeof()` fallback through `ctype_info_acq()` and
  `ctype_size_acq()` for VLA detection and fixed-size decisions.
- Routed the parser-lock `ffi.offsetof()` fallback through `ctype_info_acq()`
  and `ctype_size_acq()` for struct eligibility and field/bitfield result
  classification.
- Documented the invariant formerly checked by `m7_ffi_typeinfo_snapshot`: raw
  `CType.info`/`CType.size` reads in the `ffi.sizeof()` and `ffi.offsetof()`
  API bodies.

Verification:

- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `git diff --check`
