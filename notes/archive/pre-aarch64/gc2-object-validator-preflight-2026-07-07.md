# GC2 object validator preflight

Date: 2026-07-07

## Summary

GC2 public object validators now prove candidate memory belongs to live registered
allocator storage before reading the GC object header. This keeps conservative
stack/table/queue snapshots from interpreting arbitrary checkptrGC-shaped words
as object headers.

## Details

- Added `lj_arena_hugetab_range_lookup()` so validators can recognize headers
  stored inside huge allocations, including variable-size/aligned cdata.
- Split GC2 validation into memory-kind preflight for custom allocator, small
  arena cell, and huge allocation cases.
- Kept queued-object small-arena owner/bitmap validation for known published
  queue edges while preserving the stricter public validator behavior.
- Hardened GC2 finalizer dispatch and FINREG cdata order resolution so stale
  queue/order entries are validated before reading `gct`.
- The FINREG stress test now keeps its shared notification channel rooted until
  worker-created cdata finalizers have drained, matching the cross-thread
  lifetime the test depends on.

## Coverage

- `t-gc2-alloc-account.c` now rejects a canonical non-object pointer through both
  public validators.
- The same test accepts a public `ffi.new('uint8_t[?]', 70000)` variable cdata
  object through both validators.
- `m7_ffi_finreg` covers stale FINREG order/queue validation under interpreted
  and JIT finalizer dispatch.
