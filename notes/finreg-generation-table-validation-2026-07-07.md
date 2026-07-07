# FINREG Generation Table Validation

FINREG generation nodes are raw side-list entries that point at weak table
generations. Several lookup, scan, mark, and close-time paths were loading the
generation table and then reading table metadata without first proving the raw
node and table object were still valid.

This slice routes those paths through a shared validator:

- validate each `FinRegGen` node with `lj_gc2_mem_registered()` before reading
  its links or table pointer;
- validate the generation table with `lj_gc2_obj_valid_queued()` and an
  acquired table tag check before table header, metatable, or node reads;
- preserve disabled-generation semantics by treating an invalid or stale head
  table like a disabled generation for new FINREG publishes;
- apply the same table validation to `lj_gc2_finreg_cdata_disable()` before it
  writes the close-time FINREG disable bit.

Valid disabled generation tables are still markable so shutdown can drain them,
while lookup and `istab` continue to skip disabled generations.

Validation:

- `tools/ci/lua_test.sh m7_ffi_finreg`
- `tools/ci/lua_test.sh m5_tab_finreg_newkey_stale`
- `tools/ci/lua_test.sh m3_gc2_paranoia`
