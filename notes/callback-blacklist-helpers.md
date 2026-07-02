Callback blacklist helper slice

- Added `ctype_cbblack_*()` helpers for CTState callback-blacklist pointer,
  size, overflow flag, slot acquire loads, and slot CAS publication.
- Routed callback blacklist initialization, C-call blacklist recording,
  recorder checks, legacy GC/GC2 memory scanning, paranoia memory checks, and
  CTState teardown through the helper API.
- Documented the invariant formerly checked by `m7_ffi_callback_runtime`: raw
  implementation-side `cts->cbblack`, `cts->sizecbblack`, and
  `cts->cbblack_all` access.

Verification:

- tools/ci/m7_ffi_callback_runtime.sh
- tools/ci/m8_weak.sh
- git diff --check
