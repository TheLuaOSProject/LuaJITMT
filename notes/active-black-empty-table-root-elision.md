# Active-Black Empty Table Root Elision

Standalone GC2 active-black empty table allocation no longer publishes the new
table through the pending-root list. The arena block bit and active mark bit are
visible before the helper or inline VM path can assist GC, and an empty table has
no child edges to retain. In that state the arena bitmap is the ownership record,
so adding the object to the legacy root spine only adds avoidable pending-root
traffic.

The elision is deliberately narrow:

- `tg->mark_active` must be set.
- `tg->alloc.alloc_black` must be set.
- `g->gc2.legacy_mark_bridge` must be clear.

Idle allocation, active-white allocation, coupled legacy bridge cycles, MT, GC
workers, accounting-flush fallbacks, and non-empty table construction keep the
existing publication path. The x64 `BC_TNEW` empty-table inline path mirrors the
C helper in DynASM source; it does not use raw instruction bytes.

Focused verification:

- `LJ_TEST_DISABLE_BUILD_CACHE=1 LUA=luajit tools/ci/lua_test.sh m5_x64_tnew_empty_inline`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 LUA=luajit tools/ci/lua_test.sh m3_gc_root_pending m2_arena_gcsweep m6_jit_fnew_bump`
- `LUA=luajit tools/ci/lua_test.sh m5_tab_struct_owner m5_tab_resize_stress`
