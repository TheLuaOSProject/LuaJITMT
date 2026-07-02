# Stability sweep 2026-07-02

Scope: look for correctness and liveness bugs before making more invasive GC/JIT
performance changes. No temporary locks, alternate runtime paths, or semantic
relaxations were added.

Clean gates:

- `tools/ci/lua_test.sh m8_finalizer_error_native_stdio m8_weak`
- M7 non-cdef FFI runtime group:
  `m7_ffi_cdata_alloc m7_ffi_jit_cnew m7_ffi_snap_restore_l m7_ffi_finreg
  m7_ffi_metatype m7_ffi_cdata_get_l m7_ffi_cdata_set_l
  m7_ffi_cdata_shared_hammer m7_ffi_carith_l m7_ffi_clib_cache
  m7_ffi_clib_ldscript m7_ffi_nested_state m7_ffi_callback_install
  m7_ffi_callback_runtime m7_ffi_ccall_native`
- M7 parser/ctype publication group:
  `m7_ffi_cdef_token m7_ffi_cdef_dup_stack m7_ffi_cparse_rollback
  m7_ffi_typeinfo_snapshot m7_ffi_ctype_intern_l
  m7_ffi_ctype_hash_publish m7_ffi_ctype_tab_retire
  m7_ffi_ctype_ticket_intern m7_ffi_ctype_name_claim
  m7_ffi_ctype_pointer_ids`
- `tools/ci/lua_test.sh m9_gc_stats m9_trace_hard_assist_cadence
  m9_bench_smoke m9_bench_regression m10_generational m9_m10_gc`
- JIT stability group:
  `m6_jit_recursive_call_unroll m6_jit_token m6_jit_gcstep_guard
  m6_jit_table_store_helper m6_jit_mcode_publish`
- Corrected continuation of the JIT group:
  `m5_jit_trace_publish m6_jit_mcode_native m6_jit_gc2_readiness
  m6_jit_xbar_xpoll m6_jit_barrier_xpoll m6_jit_flush_hs
  m6_jit_alloc_account m6_jit_aref_pair_guard m6_jit_href_nodehdr
  m6_jit_hrefk_nodehdr m6_dispatch_redispatch m6_jit`
- `tools/ci/lua_test.sh run_stock_tests` passed 509/509 vendored stock tests.
- 5-run flake loop over `m5_gc_total_atomic m3_safepoint_handshake
  m4_threading_litmus` passed.

The only command-level issue was a stale local test name in the first JIT batch:
`m6_jit_trace_publish` does not exist. The trace publication gate is
`m5_jit_trace_publish`; that corrected gate passed.

Result: no reproducible stability defect surfaced in these gates. The next
highest-value stability work should either increase stress around already-known
hot structures (GC root publication, table resize/retire, JIT trace flush), or
move to a permanent implementation cleanup from the audit list with the same
gate set rerun afterward.
