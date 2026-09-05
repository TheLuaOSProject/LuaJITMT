# GC Root Pending Empty Flush Fast Path

2026-07-04

- `lj_gc_flush_root_pending()` now uses an acquire-load zero-hint fast return
  before exchanging `global_State.gcroot_pending_hint`.
- This removes a global RMW from empty pending-root flushes, which are common at
  GC/root-scan boundaries after earlier calls have already drained the current
  TGs.
- When the hint is non-zero, the function still clears it before scanning so
  concurrent publishers can republish non-empty state. A publisher racing after
  a zero-hint load is equivalent to the old `xchg(0)==0` race: it leaves the
  hint set for the next flush, while main-TG and TLS-current pending stacks are
  still checked directly before the fast return.
- The flush loop also records whether the TLS-current TG was already seen in
  `gc2.tg_list`, avoiding a redundant pair of pending-stack exchanges for
  ordinary attached secondary TGs.
