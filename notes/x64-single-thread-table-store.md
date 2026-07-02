x64 single-thread table-store lowering
=====================================

2026-07-02

- x64 JIT table stores now keep the stock `ASTORE`/`HSTORE` lowering while
  `global_State.mt_active` is still zero. This removes the helper/CAS tax for
  ordinary single-thread traces without weakening the shared-table path.
- `threading.spawn()` now flushes existing traces before the first successful
  `mt_active` transition and before the child handoff is published. This
  prevents a trace assembled with single-thread table-store assumptions from
  running after secondary Lua threads can resize/read the same table.
- After `mt_active` is latched, the existing helper/CAS lowering remains in
  force for non-trace-local table stores, preserving the FORWARD/migration and
  racy no-tear contracts.
- x64 interpreter `TSETV`/`TSETB`/`TSETR`/existing-key `TSETS` now take a
  single-thread direct store only for established non-nil current-generation
  slots. The VM still falls back to the parent/key-aware helpers when MT has
  activated, GC2 marking is active, the table is weak or has a metatable, the
  array/hash generation is retiring, a FORWARD slot is observed, or the old
  value is nil and `__newindex` semantics must be preserved.

Verification guard:

- `m6_jit_table_store_helper` now checks that single-thread dumps omit the MT
  helper/CAS route and that explicitly activated-MT dumps still contain it.
- `m6_jit_mt_activation_flush` checks that pre-MT traces are flushed by the
  first `threading.spawn()` activation.
- `m5_x64_tset_nil_snapshot` covers interpreter `__newindex`, nil-slot helper
  fallback, and forwarded-slot rerouting for the affected TSET bytecodes.
