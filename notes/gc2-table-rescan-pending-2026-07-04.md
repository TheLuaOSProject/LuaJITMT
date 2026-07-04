# GC2 Table Rescan Pending Bit

Active table barriers must keep the tri-color invariant when a marked table is
mutated while GC2 is tracing. The first write after a worker starts traversing a
table publishes a table rescan through the SSB. Later writes to the same table do
not need to publish duplicate rescans until that traversal starts.

This uses `LJ_GC_NEEDSCAN` as a table-local "rescan already queued" bit. The bit
was already a non-color GC object flag used for thread owner stack handoff, and
tables do not use the high flag bit for weak/finalizer state. The table path
therefore keeps the state on the object header instead of adding a side table or
a lock.

Rules:

- `gc2_barrier_tab_mark()` sets the bit only when it successfully queues the
  first active rescan for an already marked table.
- `gc2_traverse_tab()` clears the bit at traversal start. Writes racing with the
  traversal can then queue one more rescan, preserving progress without letting
  every store add another grey traversal.
- SSB discard and major mark-begin grey reset clear the bit for discarded table
  entries. Stale work must not suppress rescans in a later cycle.

The focused M9 guard is `m9_table_rescan_pressure`. Before this fix a single
10k-element repeated table-fill workload produced tens of thousands of
`worker_ssb_converted`/`worker_grey_drained` increments and could make a forced
full collection take seconds. After coalescing, the same workload is bounded to
the number of table rescans, not the number of table stores.
