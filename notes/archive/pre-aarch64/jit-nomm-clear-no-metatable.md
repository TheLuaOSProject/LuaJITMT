# JIT Nomm Clear With No Metatable

Traced table stores no longer emit `tab.nomm = 0` when the recorded table has
no metatable. The trace already guards `tab.meta == NULL`; if a metatable is
installed later, the guard exits and the runtime metatable publication path
clears `nomm` before the metatable can be observed.

For tables that do have a metatable, stores with possible metamethod-name keys
still clear `nomm` on trace. This preserves the negative-cache invalidation
contract while avoiding a repeated loop store in the common no-metatable
hash-write path.

Coverage:

- `m5_nomm_cache` covers runtime negative-cache publication behavior.
- `m6_jit_table_store_helper` covers traced table-store routing.
- `m9_bench_stock_compare` with `tab_hash_write` checks the performance row
  that exposed the repeated `nomm` store in x64 traces.
