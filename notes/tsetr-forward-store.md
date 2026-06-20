# x64 TSETR Forward Store Bridge

## 2026-06-20

Scoped gap addressed:

- `vmeta_tsetr` resolved integer table stores with `lj_tab_setinth()` and then
  jumped to `BC_TSETR_Z`, which still called generic `lj_tab_storetv()`.
- That left a post-lookup `TValue *` store without parent/key context, so a
  concurrent resize window could overwrite a forwarded/retiring old array slot
  instead of re-resolving into the successor generation.

Implementation:

- `vmeta_tsetr` now saves the integer key in `TMP1d` before `lj_tab_setinth()`.
- The main `BC_TSETR` fast path also saves the integer key before converting
  `RC` into an array-slot address.
- `BC_TSETR_Z` now calls `lj_tab_storetv_forvm_array(L, parent, dst, src, key)`
  instead of `lj_tab_storetv(L, dst, src)`, matching the parent-aware CAS/reroute
  path used by `TSETV`/`TSETB`/normal `TSETR`.
- `SAVE_PC` is set before the helper call because the helper can re-enter the
  C table setter path.

Verification:

- `make -C src -j$(nproc)`
- `tools/ci/lua_test.sh m5_x64_tset_nil_snapshot`
- `tools/ci/lua_test.sh m5_tab_cas_store`
- `tools/ci/lua_test.sh m6_jit_table_store_helper`
- `tools/ci/lua_test.sh m5_x64_tgets_node_order`

Related pending gap from the M10 audit:

- Public generational minor cycles appear to enter `gc_mark_start()` and get
  full legacy-root GC2 marks before the latched minor-root scanner. Next scoped
  target is to suppress the legacy-to-GC2 mark bridge when `cycle_roots_minor`
  is active, while preserving legacy marking, plus an end-to-end public minor
  cycle test.
