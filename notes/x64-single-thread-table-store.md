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

Verification guard:

- `m6_jit_table_store_helper` now checks that single-thread dumps omit the MT
  helper/CAS route and that explicitly activated-MT dumps still contain it.
- `m6_jit_mt_activation_flush` checks that pre-MT traces are flushed by the
  first `threading.spawn()` activation.
