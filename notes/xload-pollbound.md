x64 XLOAD poll-bound aliasing
=============================

2026-07-02

- Added `IRXLOAD_POLLBOUND` for raw loads whose value is tied to VM metadata
  that can be invalidated by an `XPOLL` boundary.
- Ordinary FFI/cdata scalar `XLOAD`s are no longer forced to treat `XPOLL` as
  an alias boundary. They still respect `XBAR`, `IR_CALLXS`, volatile loads,
  raw stores, and normal raw-memory alias analysis.
- `TabArrayHdr.asize` loads emitted for shared separated table arrays are marked
  poll-bound, preserving the resize/generation safety contract for AREF/ALOAD
  traces.

Observed effect:

- `ffi_struct` now forwards/hoists scalar field loads across the loop poll and
  measures at stock speed in focused local probes.
- `m6_jit_xbar_xpoll` covers both variable-index FFI loads that still remain
  after `XPOLL` and scalar struct loads that should not.
- `m6_jit_aref_pair_guard` covers the poll-bound table-array header path.
