FFI miscmap helper surface

- Added `ctype_miscmap_acq()` and `ctype_miscmap_rel()` for the remaining
  CTState misc table root.
- Routed function-pointer metamethod lookup, miscmap slot storage, and
  `luaopen_ffi()` miscmap publication through the helper API.
- Extended `tools/ci/m7_ffi_metatype.sh` to reject raw implementation-side
  `cts->miscmap` access alongside the metatype side-map fields.

Verification:

- tools/ci/m7_ffi_metatype.sh
- tools/ci/m0_source_guard.sh
- git diff --check
