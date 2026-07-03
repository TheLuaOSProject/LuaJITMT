FFI layout fallback helper loads

- Routed the parser-lock `ffi.sizeof()` fallback through `ctype_info_acq()` and
  `ctype_size_acq()` for VLA detection and fixed-size decisions.
- Routed the parser-lock `ffi.offsetof()` fallback through `ctype_info_acq()`
  and `ctype_size_acq()` for struct eligibility and field/bitfield result
  classification.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_typeinfo_snapshot` behavior/counter fixtures and code-adjacent helper docs; raw-field source inventories are not pass/fail contracts.

Verification:

- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `git diff --check`
