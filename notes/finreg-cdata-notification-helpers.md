# FINREG Cdata Notification Helpers

## Summary

Producer-side cdata FINREG notifications now enter through GC2-owned public
helpers:

- `lj_gc2_finreg_cdata_note_sweep_queued()` records the fatal sweep/free
  invariant tripwire before `lj_cdata_free()` aborts a finalized cdata free.
- `lj_gc2_finreg_cdata_note_order_retired()` records ordered FINREG retire
  accounting when `lj_ctype_fin_order_retire()` moves a node to the retired
  list.

The low-level `gc2_finreg_cdata_*_add()` counter helpers remain the storage
primitive inside `lj_gc2.c`; cdata and ctype producers no longer own those
counter writes directly.

## Invariant check

`tools/ci/m7_ffi_finreg.sh` requires both notification helpers and rejects
direct `gc2_finreg_cdata_sweep_queued_add()` or
`gc2_finreg_cdata_order_retired_add()` calls from `src/lj_cdata.c` and
`src/lj_ctype.c`.
