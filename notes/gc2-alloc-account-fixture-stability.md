# GC2 allocation-account fixture stability

`tests/t-gc2-alloc-account.c` had stale expectations around idle
generational publication and active minor allocation color.

- Active minor cycles enable alloc-black through the TG handshake, so a table
  allocated during MARK is born with its GC2 arena mark bit set.
- Lua stack API publication in idle generational mode can conservatively queue
  a remembered root. Sweep-only fixture cases now use `lj_tab_new()` internal
  allocation when they need an object that is linked in the allocation chain
  but not stack-published into the remembered set.
- VM/JIT table-store remembered-set assertions now sample counters after
  argument stack publication and before cleanup `lua_pop()`, because
  `lua_settop()` republishes the visible stack range in this fork.
- Helper-specific remembered-set blocks allocate fresh unmarked children after
  each SSB drain, preserving the intended old-parent/young-child checks.

Verification:

- `tools/ci/lua_test.sh m6_jit_alloc_account`
- `tools/ci/lua_test.sh m2_arena_gcsweep`
- `tools/ci/lua_test.sh m9_m10_gc`
- `tools/ci/lua_test.sh run_stock_tests -- --quiet`
