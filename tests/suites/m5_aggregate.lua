local runtime = require("suite_runtime")

local m5_concurrent_cases = {
  "m5_nbtab_model",
  "m5_tab_emptyhash",
  "m5_tab_nodehdr",
  "m5_tab_retire",
  "m5_tab_chain_order",
  "m5_tab_node_publish",
  "m5_tab_array_publish",
  "m5_tab_slot_snapshot",
  "m5_tab_keylock_lookup",
  "m5_tab_forward_filter",
  "m5_tab_cas_store",
  "m5_tab_value_publish",
  "m5_strtab_prep",
  "m5_strtab_cas",
  "m5_itype_nan",
  "m5_itype_sentinel",
  "m5_math_random_tg",
  "m5_udtype_publish",
  "m5_ctype_name_publish",
  "m5_registry_root",
  "m5_nomm_cache",
  "m5_os_reentrant",
  "m5_state_owner",
  "m5_buffer_publish",
  "m5_threading_alloc",
  "m5_jit_trace_publish",
  "m5_hookmask_atomic",
  "m5_hook_state_atomic",
  "m5_gc_total_atomic",
  "m5_proto_kgc_acq",
  "m5_proto_chunkname_acq",
  "m5_proto_knum_acq",
  "m5_jit_table_fload_mutable",
  "m5_jit_hash_store_nyi",
  "m5_jit_href_node_order",
  "m5_jit_hrefk_record_snapshot",
  "m5_x64_table_next_snapshot",
  "m5_x64_itern_snapshot",
  "m5_x64_ipairs_snapshot",
  "m5_x64_tget_array_header",
  "m5_x64_tgets_node_order",
  "m5_x64_getmetatable_node_order",
  "m5_x64_tset_nil_snapshot",
  "m5_parser_capture_meta",
  "m5_bcdump_compat",
  "m5_upvalue_publish_gc",
  "m5_cell_ops"
}

return function(add)
  add({
    name = "m5_concurrent_objects",
    description = "M5 concurrent-object aggregate scaffold gates",
    deps = m5_concurrent_cases,
    run = function(t)
      runtime.run_lua_test_cases(t, m5_concurrent_cases)
      print("M5 concurrent-object scaffold tests passed")
    end
  })
end
