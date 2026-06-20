CType retired table head helpers

- Added `ctype_retiredtab_acq()`, `ctype_retiredtab_cas()`, and
  `ctype_retiredtab_xchg_acqrel()` for the CTState retired CTypeTab list head.
- Routed table retirement push, epoch reclaim, final free, legacy GC memory
  marking, GC2 memory marking, and GC2 paranoia scans through the helper API.
- Extended `tools/ci/m7_ffi_ctype_tab_retire.sh` to reject raw
  implementation-side `cts->retiredtab` access.

Verification:

- tools/ci/m7_ffi_ctype_tab_retire.sh
- tools/ci/m7_ffi_ctype_ticket_intern.sh
- tools/ci/m0_source_guard.sh
- git diff --check
