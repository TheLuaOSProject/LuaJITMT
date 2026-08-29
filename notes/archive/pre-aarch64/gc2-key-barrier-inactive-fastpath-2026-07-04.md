# GC2 key-barrier inactive fast path

Date: 2026-07-04

`lj_gc2_barrier_key_g()` now returns before phase and remembered-set checks when
the global state has no thread group or the thread group is not actively
marking.

This keeps the table publication boundary unchanged. New hash-key insertion
still calls `lj_gc2_barrier_weak_key()` and `lj_gc_pubtabkey()`, and
`lj_gc_pubtabkey()` still performs the legacy incremental back barrier for a
white key inserted into a black table.

The fast path is helper-internal because both GC2 actions behind the helper
already require `mark_active`:

- `gc2_barrier_active_g()` only marks while the thread group is mark-active and
  the phase is MARK or WEAK.
- `gc2_remember_pair()` reaches `gc2_remember_active_g()`, which only records a
  remembered pair while the phase is IDLE, generational mode is enabled, and the
  thread group is mark-active.

Keeping this decision in `lj_gc2_barrier_key_g()` avoids reintroducing the
unsafe call-site shortcuts that previously skipped table publication work during
new-key insertion.
