# JIT pre-MT primitive table-store direct path

Single-thread traces may use the stock x64 `ASTORE`/`HSTORE` lowering for
existing-slot primitive and number stores. These stores do not create GC-object
value edges, and the recorder still emits the normal table, metamethod, and
barrier checks around the store. `NEWREF` and GC-object value stores remain on
the helper path.

The boundary is `mt_entering || mt_active`: once another Lua thread is entering
or already live, published table stores must use the CAS/helper route so resize,
weak-table, and retired-node validation happens outside raw trace stores. The
first MT activation flushes existing traces before secondary Lua threads run,
which makes pre-MT direct stores safe without leaving stale single-thread traces
in the multi-threaded phase.

Verification:

- `m6_jit_table_store_helper` checks that pre-MT primitive stores emit `ASTORE`
  and `HSTORE` without `lock cmpxchg` or `lj_tab_storetv_forjit*`.
- The same test keeps active-MT published primitive stores on the inline
  CAS/helper route, and keeps GC-object stores helper-backed.
