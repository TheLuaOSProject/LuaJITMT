CType retired table head helpers

- Added `ctype_retiredtab_acq()`, `ctype_retiredtab_cas()`, and
  `ctype_retiredtab_xchg_acqrel()` for the CTState retired CTypeTab list head.
- Routed table retirement push, epoch reclaim, final free, legacy GC memory
  marking, GC2 memory marking, and GC2 paranoia scans through the helper API.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_ctype_tab_retire` behavior/counter fixtures and code-adjacent helper docs; raw-field source inventories are not pass/fail contracts.

Verification:

- tools/ci/m7_ffi_ctype_tab_retire.sh
- tools/ci/m7_ffi_ctype_ticket_intern.sh
- git diff --check
