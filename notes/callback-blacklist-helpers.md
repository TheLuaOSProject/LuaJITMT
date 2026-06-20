Callback blacklist helper slice

- Added `ctype_cbblack_*()` helpers for CTState callback-blacklist pointer,
  size, overflow flag, slot acquire loads, and slot CAS publication.
- Routed callback blacklist initialization, C-call blacklist recording,
  recorder checks, legacy GC/GC2 memory scanning, paranoia memory checks, and
  CTState teardown through the helper API.
- Extended `tools/ci/m7_ffi_callback_runtime.sh` to reject raw
  implementation-side `cts->cbblack`, `cts->sizecbblack`, and
  `cts->cbblack_all` access.

Verification:

- tools/ci/m7_ffi_callback_runtime.sh
- tools/ci/m8_weak.sh
- tools/ci/m0_source_guard.sh
- git diff --check
