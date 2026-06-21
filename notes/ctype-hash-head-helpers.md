CType hash head helpers

- Added `ctype_hash_head_acq()` and `ctype_hash_head_cas()` for CTState hash
  bucket heads.
- Routed the local ctype hash load/CAS helpers through the shared helper API,
  preserving the low-16-bit CTypeID payload and CAS-prepend semantics.
- Extended `tools/ci/m7_ffi_ctype_hash_publish.sh` to reject raw
  implementation-side `cts->hash` access.

Verification:

- tools/ci/m7_ffi_ctype_hash_publish.sh
- tools/ci/m7_ffi_ctype_ticket_intern.sh
- tools/ci/m0_source_guard.sh
- git diff --check
