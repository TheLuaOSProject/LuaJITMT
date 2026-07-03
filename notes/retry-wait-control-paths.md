Retry wait control-path pass
============================

- Parser keep-anchor, serializer dictionary, and GDBJIT descriptor retry waits
  now use `lj_thr_retry_yield(L)` instead of blind 1ms native sleeps. These are
  short CAS/forwarding retry windows with a current Lua state, so they should
  stay safepoint-visible without millisecond parking.
- `jit.profile` lifecycle waits were not converted to spin/yield-only. The
  concurrent start/stop stress gate showed that pure yielding turns the global
  profiler state transition into CPU-heavy contention and can hit the join
  timeout.
- The profiler lifecycle path now waits on the concrete futex word
  (`ProfileState.state` or `ProfileState.callbacks`) while the TG is in native
  state. This removes blind sleeping, wakes promptly on state/callback release,
  and keeps the control path separate from warm mutator/JIT/FFI retry loops.
- Table structural ownership is per-table, not universe-global. Same-table
  structural contention still serializes resize/compound array mutation, and
  the wait parks on the table's `GCtab.struct_owner` futex word. Independent
  tables do not serialize with each other; contested same-table waits use the
  concrete owner-release wake instead of blind sleeping.
- Table `next()` skips transient hidden hash keys (`KEYLOCK` or unpublished nil
  key) with a second acquire snapshot rather than calling the generic table retry
  helper. This keeps traversal read-side behavior nonblocking while preserving
  the rule that internal sentinels are never exposed to Lua.
