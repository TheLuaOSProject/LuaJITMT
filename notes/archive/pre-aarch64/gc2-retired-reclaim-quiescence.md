# GC2 retired reclaim quiescence

Retired raw structures are still traversed by both collectors until they are
physically reclaimed:

- string-table headers from `gc*_mark_strtab_mem`;
- table node/array retirement records from `gc*_mark_tab_retired_mem`;
- ctype retired tables and CLibrary side-cache entries from FFI root scans;
- retired trace vectors/bodies from `lj_trace_markvecs`;
- retired mcode records from `lj_mcode_markretired`.

The per-domain reclaim functions detach retired lists with `xchg`, clear
`next`, and then free or requeue nodes based on the completed safepoint epoch.
That is safe for ordinary mutator readers after a grace epoch, but it is not
safe to run while a collector is traversing the retired lists as roots: a marker
can observe a node that reclaim concurrently unlinks or frees.

The central fix has two parts. First, `lj_gc2_reclaim_retired()` claims a single
`smr_reclaiming` flag before running the domain fanout. Then it rechecks
collector quiescence and waits for `smr_readers` to reach zero. Second, legacy
and GC2 global-root scans enter an SMR read section before walking retired
lists. A marker that starts while reclaim is active waits before it can read
those lists; a reclaim that starts while a marker is already reading waits
before it can detach/free nodes.

Physical SMR drain still only runs when:

- legacy GC is paused;
- GC2 phase is idle;
- no GC2 worker, assist, or weak-drain/write owner is active.

This keeps the existing safepoint-epoch retire protocol and does not add a new
mutator lock or table/string/JIT hot-path branch. Reclaim is deferred until
retired raw memory is no longer part of any active root traversal.

Validation:

- `make -C src -j$(nproc)`
- `LUA=src/luajit tools/ci/lua_test.sh m5_strtab_cas m5_jit_trace_publish m7_ffi_ctype_tab_retire`
- `LUA=src/luajit tools/ci/lua_test.sh m3_safepoint_handshake`
- `LUA=src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet` (509 passed)
