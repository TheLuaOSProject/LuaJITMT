Metatype side-map helper slice

- Added `ctype_metamap_*()` helpers for CTState metatype side-root pointer,
  size publication, slot acquire loads, and one-shot slot CAS publication.
- Routed `ffi.metatype()` publication, metatype lookup, legacy GC marking, GC2
  marking, paranoia memory checks, and CTState teardown through the helper API.
- Documented the invariant formerly checked by `m7_ffi_metatype`: raw implementation-side
  `cts->metamap` and `cts->sizemeta` access.

Verification:

- tools/ci/m7_ffi_metatype.sh
- tools/ci/m8_weak.sh
- git diff --check
