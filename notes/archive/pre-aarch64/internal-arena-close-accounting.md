# Internal Arena Close Accounting

- The assert/paranoia stock-suite close failure was an accounting artifact, not
  a live-object leak: Valgrind reported all heap blocks freed, while
  `g->gc.total` still included memory owned by the internal arena allocator.
- The previous close normalization only handled the `mt_active != 0` case. A
  single-thread stock-suite run can leave the same allocator-owned arena
  accounting visible until `lj_arena_alloc_fini()` destroys the internal arena.
- `close_state()` now normalizes `g->gc.total` to `sizeof(GG_State)` for any
  internal-arena close once roots, strings, traces, FFI state, retired tables,
  GC2 side structures, buffers, stack, and lightuserdata segments have already
  been released.
- Verification:
  - `make -C src clean && make -C src -j2`
  - `tools/ci/lua_test.sh m3_gc2_paranoia`
  - direct assert/paranoia stock-suite run after the gate rebuild:
    `src/luajit test.lua --quiet` from `tests/stock/test`
  - `tools/ci/lua_test.sh m2_arena_gcclose`
