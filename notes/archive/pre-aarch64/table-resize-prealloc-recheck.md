# Table Resize Preallocation Recheck

`lj_tab_resize()` now rechecks the table array/hash generation after computing
the requested shape and before allocating replacement storage. A competing
mutator can publish the requested generation in that sizing window; restarting
there avoids allocating and immediately freeing stale replacement arrays, hash
parts, and retire records.

This is only an early retry filter. The existing owner-side snapshot recheck
remains the authoritative guard before publishing a new generation, because the
table can still change after allocation and before the structural owner is
acquired.

Focused verification:

- `LJ_TEST_DISABLE_BUILD_CACHE=1 LUA=luajit tools/ci/lua_test.sh m5_tab_struct_owner m5_tab_resize_stress m5_x64_tgets_node_order m5_x64_tset_nil_snapshot`
