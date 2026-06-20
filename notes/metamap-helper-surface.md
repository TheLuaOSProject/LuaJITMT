Metatype side-map helper slice

- Added `ctype_metamap_*()` helpers for CTState metatype side-root pointer,
  size publication, slot acquire loads, and one-shot slot CAS publication.
- Routed `ffi.metatype()` publication, metatype lookup, legacy GC marking, GC2
  marking, paranoia memory checks, and CTState teardown through the helper API.
- Extended `tools/ci/m7_ffi_metatype.sh` to reject raw implementation-side
  `cts->metamap` and `cts->sizemeta` access.

Verification:

- tools/ci/m7_ffi_metatype.sh
- tools/ci/m8_weak.sh
- tools/ci/m0_source_guard.sh
- git diff --check
