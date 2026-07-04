# Table Resize No-Op Owner Bypass

`lj_tab_resize()` now returns before acquiring `GCtab.struct_owner` when the
requested array size and hash generation already match the published table
generation. This removes a redundant per-table structural-owner acquisition
from racing or repeated resize requests.

The bypass deliberately excludes same-sized hash rebuilds that see dead value
slots. Those rebuilds compact stale hash keys and recover future insertion
capacity, so skipping them would change table growth behavior even though the
nominal hash size is unchanged.

Validation:

- `tools/ci/lua_test.sh m5_tab_struct_owner`
- `tools/ci/lua_test.sh m5_tab_keylock_lookup m5_tab_forward_filter m5_tab_retire m5_tab_capi_resize_stress m5_tab_resize_stress m5_tab_colocated_resize m5_tab_finreg_newkey_stale m5_tab_next_snapshot`
- `tools/ci/lua_test.sh m6_jit_table_store_helper`
- `LJ_BENCH_STOCK_SCALE=0.2 tools/ci/lua_test.sh m9_bench_stock_compare`
