# Bump Allocation Accounting Boundary

The pre-MT bump helpers for empty tables, no-upvalue closures, one-local-cell
closures, and closed nil upvalue cells keep their strict single-producer
predicates. They no longer treat a full `TG.local_total` batch as a reason to
fall all the way back to generic allocation.

When the next bump allocation would cross `LJ_GC2_ACCT_FLUSH`, the helper
allocates from the current bump run, updates `gc.total`, then accounts that
allocation through `lj_gc2_account_alloc()`. That preserves the existing GC2
trigger and hard-assist behavior for the allocation itself while avoiding a
generic allocator round trip on a deterministic accounting-boundary miss.

The x64 inline paths still jump to the C helper at this boundary. The C helper
now owns the flush and can return a normal bump-allocated object after the
required pacing work. This keeps the machine-code fast path simple and keeps
GC2 pacing in one C implementation.

Coverage:

- `m6_jit_fnew_bump` verifies one-upvalue numeric FNEW identity and accounting
  boundary behavior.
- `m5_x64_tnew_empty_inline` verifies empty-table boundary flushing and
  published table shape.
- `m6_jit_alloc_account` keeps the lower-level `lj_gc2_account_alloc()`
  trigger and hard-assist contract covered.
