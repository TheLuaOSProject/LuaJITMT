FFI miscmap helper surface

- Added `ctype_miscmap_acq()` and `ctype_miscmap_rel()` for the remaining
  CTState misc table root.
- Routed function-pointer metamethod lookup, miscmap slot storage, and
  `luaopen_ffi()` miscmap publication through the helper API.
- Documented the invariant formerly checked by `m7_ffi_metatype`: raw implementation-side
  `cts->miscmap` access alongside the metatype side-map fields.

Verification:

- tools/ci/m7_ffi_metatype.sh
- git diff --check
