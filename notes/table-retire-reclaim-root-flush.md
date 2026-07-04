# Table Retire Reclaim Root Flush

Date: 2026-07-04

`lj_tab_reclaim_retired()` still keeps the conservative published-root scan
before physically freeing retired table arrays or hash vectors. That scan is
cold-path validation: if a retired generation is still the root currently
published by a live table, reclaim must leave it queued.

Pending roots only need to be flushed once before the reclaim batch starts.
Draining `TGState.gcroot_pending` for every retired node/array repeated the same
global work and made batched table-generation reclaim scale with both retired
records and pending-root drains. The still-published helpers now only walk the
root list; the reclaim driver performs the pending-root flush once before
processing node and array retirement lists.

Coverage: `m5_tab_retire` and `m5_tab_array_publish` now temporarily republish a
retired generation and verify that elapsed reclaim leaves it queued until the
table root is restored to the current generation.
