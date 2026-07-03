Callback blacklist helper slice

- Added `ctype_cbblack_*()` helpers for CTState callback-blacklist pointer,
  size, overflow flag, slot acquire loads, and slot CAS publication.
- Routed callback blacklist initialization, C-call blacklist recording,
  recorder checks, legacy GC/GC2 memory scanning, paranoia memory checks, and
  CTState teardown through the helper API.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_callback_runtime` behavior/counter fixtures and code-adjacent helper docs; raw-field implementation-text inventories are not pass/fail contracts.

Verification:

- tools/ci/m7_ffi_callback_runtime.sh
- tools/ci/m8_weak.sh
- git diff --check
