# Table new-key private publication fast path (2026-07-04)

`lj_tab_newkey()` now has a private single-mutator path for hash-node
publication.  The path is enabled only when:

- no Lua thread is live or entering (`mt_active_or_entering_acq(g) == 0`);
- no GC2 workers are running (`gc2_n_workers_acq(g) == 0`);
- the current thread group is outside active marking
  (`lj_tg_mark_active_acq(L2TG(L)) == 0`).

Under those conditions the current mutator is the only observer of the hash
vector.  The path still preserves the shared-path accounting and barriers:
freecount is decremented for the reserved slot, `tab_storekeyrel()` canonicalizes
the key, `lj_gc2_barrier_weak_key()` handles weak-key phase publication, and
`lj_gc_pubtabkey()` preserves the GC2 key edge plus the legacy back barrier.

Measured result:

- `tools/ci/lua_test.sh m9_bench_stock_compare`: `tab_insert_newkey` moved from
  roughly `2.03x` stock before this change to `1.92x` in the guard run.
- Full-scale focused run after the patch: fork `143.04 ns/op`, stock
  `/usr/bin/luajit` `98.39 ns/op` (`~1.45x` on that run).

A broader private keyed-value-store shortcut was tested and rejected.  Directly
replacing `lj_tab_trystoretv_cas_keyed()` with a private copy after current-slot
validation broke `m5_tab_colocated_resize`: stale colocated array slots must still
be observed as `FORWARD` by the CAS/validation path.  Future store fast paths need
to prove array/hash generation and forwarding invariants more narrowly, not just
reuse the new-key private predicate.
