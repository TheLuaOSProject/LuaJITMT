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

## Coverage

`m7_ffi_finreg` owns the observable FINREG behavior. Cdata and ctype producers
must enter through the public notification helpers rather than writing the
low-level counters directly; that ownership rule is documented here and beside
the helper surface instead of in a source-text predicate.
