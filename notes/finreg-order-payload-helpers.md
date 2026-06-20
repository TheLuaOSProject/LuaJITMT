2026-06-20

- Added `fin_order_tab_*()` and `fin_order_slot_*()` helper accessors beside
  the existing `fin_order_obj_*()` helpers for `FinRegOrderNode`.
- `lj_ctype_fin_order_new()` and `lj_ctype_fin_order_publish()` now release
  initialize/publish the ordered FINREG payload through those helpers before
  the node is CAS-prepended to `CTState.fin_order_head`.
- Ordered P_WEAK, close-time cdata separation, and close-time pending scans now
  acquire-load `tab` and `slot` through the helper API instead of open-coded
  pointer casts.
- Extended `tools/ci/m7_ffi_finreg.sh` so raw ordered-node `obj`/`tab`/`slot`
  access in production FINREG code is rejected.
- Validation: `tools/ci/m7_ffi_finreg.sh` and `tools/ci/m8_weak.sh`.
