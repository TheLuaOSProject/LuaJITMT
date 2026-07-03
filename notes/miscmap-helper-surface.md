FFI miscmap helper surface

- Added `ctype_miscmap_acq()` and `ctype_miscmap_rel()` for the remaining
  CTState misc table root.
- Routed function-pointer metamethod lookup, miscmap slot storage, and
  `luaopen_ffi()` miscmap publication through the helper API.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_metatype` behavior/counter fixtures and code-adjacent helper docs; raw-field implementation-text inventories are not pass/fail contracts.

Verification:

- tools/ci/m7_ffi_metatype.sh
- git diff --check
