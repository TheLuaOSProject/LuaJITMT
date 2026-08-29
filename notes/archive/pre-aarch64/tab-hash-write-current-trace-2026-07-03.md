Table hash-write current trace evidence, 2026-07-03:

- The pre-MT `tab.meta` XPOLL forwarding change is active. Current traced
  benchmark-shaped hash writes no longer carry a repeated post-XPOLL metatable
  load.
- The steady-state traced store is a direct numeric `HSTORE` after `HREF` and a
  nil-slot guard. It is not routed through `lj_tab_storetv_forjit_hash`, and the
  hot existing-slot trace does not pay the table-store CAS helper path.
- Pre-activation loop XPOLL elision removes the loop TG-poll component from
  single-thread benchmark-shaped traces. The remaining gap against stock is
  currently attributed to lockless runtime overhead outside the final store:
  the larger GC2 allocation/assist check sequence, TG-owned tmpbuf/string
  construction for the benchmark key path, node-header hash-mask loads,
  string-table publication, and first-fill/resize publication work.
- The benchmark harness now keeps the historical `tab_hash_write` row intact,
  but adds split rows for attribution:
  `tab_store_existing` uses prebuilt string keys and prefilled slots so it
  measures the steady-state `HREF`/`HSTORE` path, and `tab_insert_newkey` uses
  unique string keys so insertion/growth costs are visible separately. This
  keeps the stock comparison from treating a mixed keybuild/insert/store row as
  evidence about one primitive.
- The durable coverage is `m6_jit_table_store_helper`,
  `m6_jit_tbar_gc2_black_gate`,
  `m6_jit_tmpbuf_thread_format`, `m6_jit_barrier_xpoll`,
  `t-jit-forward-store`, `t-jit-entering-table-store`,
  `t-jit-tg-tmpbuf-reset`, and stock/bench comparison runs.
- A future pre-MT-only tmpbuf or node-header optimization may be possible, but
  it needs behavior tests for MT activation flush, TG tmpbuf ownership, table
  resize generation changes, and stock Lua table/string semantics before any
  implementation change.
