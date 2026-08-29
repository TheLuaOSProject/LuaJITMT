# x64 iterator retiring-array coverage

The earlier x64 forwarding fixtures covered separated-array `FORWARD` slots,
but mostly did not pin down the window where a table still points at the old
array root while `TABARRAY_FLAG_RETIRING` is set and the successor is already
published. That window matters for iterator paths because stale non-forward
slots must not leak to Lua while a resize is being completed by another thread.

`tests/t-x64-ipairs-forward.c`, `tests/t-x64-itern-forward.c`, and
`tests/t-x64-vm-next-forward.c` now each exercise that current-root retiring
array case:

- the old array contains a stale non-forward value at the target slot;
- the new array contains the value Lua should observe;
- the old array is restored as the table root with `RETIRING` set;
- a helper pthread publishes the successor array after a short delay;
- the iterator path must wait/follow through the existing snapshot machinery
  and return the successor value, not the stale old root value.

This is test-only coverage for the existing iterator guards and does not add a
new runtime path or lock.

Validation:

- `make -C src -j$(nproc)`
- `LUA=src/luajit tools/ci/lua_test.sh m5_x64_ipairs_snapshot m5_x64_itern_snapshot m5_x64_table_next_snapshot`
- `LUA=src/luajit tools/ci/lua_test.sh m5_x64_getmetatable_node_order m5_x64_tget_array_header m5_x64_tgets_node_order m5_x64_ipairs_snapshot m5_x64_itern_snapshot m5_x64_table_next_snapshot m5_x64_tset_nil_snapshot`
- `LUA=src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet` (509 passed)
