# x64 TGET retiring-array guard

The x64 interpreter `BC_TGETV`, `BC_TGETB`, and `BC_TGETR` fast paths loaded
from separated array storage after reading the array header size, but did not
check `TABARRAY_FLAG_RETIRING`. A table resize can temporarily leave the old
array as the current table root with `next_gen` published and `RETIRING` set
before every old slot has become `FORWARD`. Reading a non-forward stale slot in
that window bypasses the existing forwarding/snapshot path.

The TGET array fast paths now mirror `ipairs_aux`, `lj_vm_next`, `BC_ITERN`,
and x64 TSET stores: separated arrays marked retiring fall through to the
existing VM metamethod/helper path. Colocated arrays keep the existing direct
path.

`tests/t-x64-tget-forward.c` now exercises a current-root retiring array with a
non-forward stale value while a helper pthread publishes the successor array.
Without the guard, the bytecode reads the stale old-generation value before the
successor publish; with the guard, it routes through the slow path and observes
the successor.

Validation:

- `make -C src -j$(nproc)`
- `LUA=src/luajit tools/ci/lua_test.sh m5_x64_tget_array_header`
- `LUA=src/luajit tools/ci/lua_test.sh m5_x64_getmetatable_node_order m5_x64_tget_array_header m5_x64_tgets_node_order m5_x64_ipairs_snapshot m5_x64_itern_snapshot m5_x64_table_next_snapshot m5_x64_tset_nil_snapshot`
- `LUA=src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet` (509
  passed)
