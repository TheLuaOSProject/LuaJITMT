FFI pin metatable helper surface

- Added `ctype_pinmt_acq()` and `ctype_pinmt_rel()` for the `ffi.pin` handle
  metatable root.
- Routed pin construction, `luaopen_ffi()` pin metatable publication, legacy
  GC/GC2 root marking, and GC2 paranoia checks through the helper API.
- Extended `tools/ci/m7_ffi_pin.sh` to reject raw implementation-side
  `cts->pinmt` access.

Verification:

- tools/ci/m7_ffi_pin.sh
- tools/ci/m8_weak.sh
- tools/ci/m0_source_guard.sh
- git diff --check
