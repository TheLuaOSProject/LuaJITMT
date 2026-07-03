FFI pin metatable helper surface

- Added `ctype_pinmt_acq()` and `ctype_pinmt_rel()` for the `ffi.pin` handle
  metatable root.
- Routed pin construction, `luaopen_ffi()` pin metatable publication, legacy
  GC/GC2 root marking, and GC2 paranoia checks through the helper API.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_pin` behavior/counter fixtures and code-adjacent helper docs; raw-field implementation-text inventories are not pass/fail contracts.

Verification:

- tools/ci/m7_ffi_pin.sh
- tools/ci/m8_weak.sh
- git diff --check
