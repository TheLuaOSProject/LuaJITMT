# JIT primitive table-store inline CAS

Linux/x64 JIT `ASTORE`/`HSTORE` had an inline CAS success path for numeric and
dual-number integer table stores after multithreading activation. Primitive
boolean/nil stores still took the helper-first fallback even though they do not
need value-side GC barriers.

The inline store gate now accepts primitive TValues. The emitted source word is
materialized from a local `TValue` initialized with the normal `setnilV()` and
`setpriV()` helpers, so the path follows the existing tagged-value encoding
instead of open-coding byte patterns. The surrounding safety checks are
unchanged: stale generations, retiring arrays/nodes, colocated arrays, weak
tables, active metatables, and CAS failure continue to route through the
existing C helpers.

Focused regression test: `m6_jit_table_store_helper` now includes a boolean-only
array/hash route dump and requires the x64 `lock cmpxchg` marker plus the same
helper fallback names.
