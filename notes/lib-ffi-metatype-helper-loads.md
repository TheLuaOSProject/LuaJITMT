lib_ffi metatype helper loads

- Routed `ffi_meta___call` through `ctype_info_acq()` before stripping pointer
  ctype wrappers for `__call`/`__new` lookup.
- Routed `ffi_meta___tostring` through `ffi_ctype_info_read()` while
  resolving references, complex/int64 formatting, function pointers, enums,
  pointer payloads, and struct/vector `__tostring` dispatch.
- Routed `ffi_pairs()` through `ctype_info_acq()` before pointer wrapper
  stripping for `__pairs`/`__ipairs` lookup.
- Routed `ffi_metatype()` through `ffi_ctype_info_read()` before validating
  metatype-eligible struct, complex, and vector ctypes.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_metatype` behavior/counter fixtures and code-adjacent helper docs; raw-field implementation-text inventories are not pass/fail contracts.

Verification:

- tools/ci/m7_ffi_metatype.sh
- tools/ci/m7_ffi_cdata_get_l.sh
- tools/ci/m7_ffi_typeinfo_snapshot.sh
- git diff --check
