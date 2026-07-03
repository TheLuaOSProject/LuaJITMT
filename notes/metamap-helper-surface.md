Metatype side-map helper slice

- Added `ctype_metamap_*()` helpers for CTState metatype side-root pointer,
  size publication, slot acquire loads, and one-shot slot CAS publication.
- Routed `ffi.metatype()` publication, metatype lookup, legacy GC marking, GC2
  marking, paranoia memory checks, and CTState teardown through the helper API.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_metatype` behavior/counter fixtures and code-adjacent helper docs; raw-field source inventories are not pass/fail contracts.

Verification:

- tools/ci/m7_ffi_metatype.sh
- tools/ci/m8_weak.sh
- git diff --check
