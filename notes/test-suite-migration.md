Lua test-suite migration notes:

- Added `tools/test.lua` plus `tests/lib/ljtest.lua` as the Lua-owned test
  runner/framework. The framework owns command execution, VM builds, C fixture
  compilation, and LuaJIT invocation.
- Test definitions live under `tests/suites/*.lua`; C files remain fixtures.
  The migration target is to make `tools/ci/*.sh` compatibility launchers only,
  with all test logic in Lua.
- Current inventory from Parfit: active custom tests are roughly 68 C fixtures
  and 37 top-level Lua tests, with 129 `tools/ci/*.sh` entrypoints. The stock
  LuaJIT suite already has a Lua runner at `tests/stock/test/test.lua`; keep
  that as a child suite rather than rewriting its internals.
- First migrated scripts:
  - `tools/ci/m2_arena_alloc.sh` -> `m2_arena_alloc`
  - `tools/ci/m4_threading_smoke.sh` -> `m4_threading_smoke`
  - `tools/ci/m5_tab_emptyhash.sh` -> `m5_tab_emptyhash`
- Second migrated scripts:
  - `tools/ci/m4_threading_litmus.sh` -> `m4_threading_litmus`
  - `tools/ci/m4_threading_stress.sh` -> `m4_threading_stress`
  - `tools/ci/m4_threading_upvalue.sh` -> `m4_threading_upvalue`
  - `tools/ci/m5_math_random_tg.sh` -> `m5_math_random_tg`
  - `tools/ci/m5_parser_capture_meta.sh` -> `m5_parser_capture_meta`
- Third migrated scripts:
  - `tools/ci/m4_threading_api.sh` -> `m4_threading_api`
  - `tools/ci/m4_threading_shutdown.sh` -> `m4_threading_shutdown`
  - `tools/ci/m5_os_reentrant.sh` -> `m5_os_reentrant`
- Fourth migrated scripts:
  - `tools/ci/m2_arena_all.sh` -> `m2_arena_all`
  - `tools/ci/m2_arena_bitmap.sh` -> `m2_arena_bitmap`
  - `tools/ci/m2_arena_map.sh` -> `m2_arena_map`
  - `tools/ci/m2_arena_hugetab.sh` -> `m2_arena_hugetab`
  - `tools/ci/m2_arena_sweep.sh` -> `m2_arena_sweep`
  - `tools/ci/m2_arena_state.sh` -> `m2_arena_state`
  - `tools/ci/m2_arena_gcmark.sh` -> `m2_arena_gcmark`
  - `tools/ci/m2_arena_gcverify.sh` -> `m2_arena_gcverify`
  - `tools/ci/m2_arena_gcclose.sh` -> `m2_arena_gcclose`
  - `tools/ci/m2_arena_gcsweep.sh` -> `m2_arena_gcsweep`
  - `tools/ci/m2_arena_gcphase.sh` -> `m2_arena_gcphase`
- Fifth batch was later removed from the runnable suite because it only
  asserted source-file shape, not behavior.
- Sixth migrated scripts:
  - `tools/ci/m4_thr_substrate.sh` -> `m4_thr_substrate`
  - `tools/ci/m4_chan_stress.sh` -> `m4_chan_stress`
  - `tools/ci/m4_threading_capi.sh` -> `m4_threading_capi`
- Seventh migrated scripts:
  - `tools/ci/m5_nbtab_model.sh` -> `m5_nbtab_model`
  - `tools/ci/m5_itype_nan.sh` -> `m5_itype_nan`
  - `tools/ci/m5_itype_sentinel.sh` -> `m5_itype_sentinel`
  - `tools/ci/m5_bcdump_compat.sh` -> `m5_bcdump_compat`
  - `tools/ci/m5_registry_root.sh` -> `m5_registry_root`
  - `tools/ci/m5_nomm_cache.sh` -> `m5_nomm_cache`
  - `tools/ci/m5_strtab_prep.sh` -> `m5_strtab_prep`
- Eighth migrated scripts:
  - `tools/ci/m5_buffer_publish.sh` -> `m5_buffer_publish`
  - `tools/ci/m5_ctype_name_publish.sh` -> `m5_ctype_name_publish`
  - `tools/ci/m5_jit_hash_store_nyi.sh` -> `m5_jit_hash_store_nyi`
- Ninth migrated scripts:
  - `tools/ci/m5_tab_slot_snapshot.sh` -> `m5_tab_slot_snapshot`
  - `tools/ci/m5_tab_keylock_lookup.sh` -> `m5_tab_keylock_lookup`
- Tenth migrated scripts:
  - `tools/ci/m5_jit_href_node_order.sh` -> `m5_jit_href_node_order`
- Eleventh migrated scripts:
  - `tools/ci/m5_tab_chain_order.sh` -> `m5_tab_chain_order`
  - `tools/ci/m5_tab_node_publish.sh` -> `m5_tab_node_publish`
  - `tools/ci/m5_tab_retire.sh` -> `m5_tab_retire`
- Twelfth migrated scripts:
  - `tools/ci/m5_udtype_publish.sh` -> `m5_udtype_publish`
- Thirteenth migrated scripts:
  - `tools/ci/m5_jit_hrefk_record_snapshot.sh` -> `m5_jit_hrefk_record_snapshot`
  - `tools/ci/m5_tab_forward_filter.sh` -> `m5_tab_forward_filter`
  - `tools/ci/m5_tab_nodehdr.sh` -> `m5_tab_nodehdr`
- Fourteenth migrated scripts:
  - `tools/ci/m5_jit_table_fload_mutable.sh` -> `m5_jit_table_fload_mutable`
  - `tools/ci/m5_threading_alloc.sh` -> `m5_threading_alloc`
- Fifteenth migrated scripts:
  - `tools/ci/m5_strtab_cas.sh` -> `m5_strtab_cas`
- Sixteenth migrated scripts:
  - `tools/ci/m3_gc2_paranoia.sh` -> `m3_gc2_paranoia`
  - `tools/ci/m3_gc2_scaffold.sh` -> `m3_gc2_scaffold`
  - `tools/ci/m3_gc2_worker_scheduler.sh` -> `m3_gc2_worker_scheduler`
  - `tools/ci/m3_safepoint_handshake.sh` -> `m3_safepoint_handshake`
  - `tools/ci/m3_vm_safepoint.sh` -> `m3_vm_safepoint`
- Seventeenth migrated scripts:
  - `tools/ci/m5_x64_getmetatable_node_order.sh` -> `m5_x64_getmetatable_node_order`
  - `tools/ci/m5_x64_ipairs_snapshot.sh` -> `m5_x64_ipairs_snapshot`
  - `tools/ci/m5_x64_itern_snapshot.sh` -> `m5_x64_itern_snapshot`
  - `tools/ci/m5_x64_table_next_snapshot.sh` -> `m5_x64_table_next_snapshot`
  - `tools/ci/m5_x64_tget_array_header.sh` -> `m5_x64_tget_array_header`
  - `tools/ci/m5_x64_tgets_node_order.sh` -> `m5_x64_tgets_node_order`
- Eighteenth migrated scripts:
  - `tools/ci/m9_gc_stats.sh` -> `m9_gc_stats`
  - `tools/ci/m9_bench_smoke.sh` -> `m9_bench_smoke`
  - `tools/ci/m9_bench_regression.sh` -> `m9_bench_regression`
  - `tools/ci/m10_generational.sh` -> `m10_generational`
  - `tools/ci/m9_m10_gc.sh` -> `m9_m10_gc`
- Nineteenth migrated scripts:
  - `tools/ci/m5_state_owner.sh` -> `m5_state_owner`
  - `tools/ci/m5_cell_ops.sh` -> `m5_cell_ops`
  - `tools/ci/m5_jit_trace_publish.sh` -> `m5_jit_trace_publish`
  - `tools/ci/m5_tab_array_publish.sh` -> `m5_tab_array_publish`
  - `tools/ci/m5_tab_cas_store.sh` -> `m5_tab_cas_store`
  - `tools/ci/m5_tab_value_publish.sh` -> `m5_tab_value_publish`
  - `tools/ci/m5_x64_tset_nil_snapshot.sh` -> `m5_x64_tset_nil_snapshot`
- Twentieth migrated scripts:
  - `tools/ci/m6_dispatch_redispatch.sh` -> `m6_dispatch_redispatch`
  - `tools/ci/m6_jit_token.sh` -> `m6_jit_token`
  - `tools/ci/m6_jit_cell_ops.sh` -> `m6_jit_cell_ops`
  - `tools/ci/m6_jit_barrier_xpoll.sh` -> `m6_jit_barrier_xpoll`
  - `tools/ci/m6_jit_xbar_xpoll.sh` -> `m6_jit_xbar_xpoll`
  - `tools/ci/m6_jit_table_store_helper.sh` -> `m6_jit_table_store_helper`
  - `tools/ci/m6_jit_aref_pair_guard.sh` -> `m6_jit_aref_pair_guard`
  - `tools/ci/m6_jit_hrefk_nodehdr.sh` -> `m6_jit_hrefk_nodehdr`
  - `tools/ci/m6_jit_href_nodehdr.sh` -> `m6_jit_href_nodehdr`
  - `tools/ci/m6_jit_alloc_account.sh` -> `m6_jit_alloc_account`
  - `tools/ci/m6_jit_gc2_readiness.sh` -> `m6_jit_gc2_readiness`
  - `tools/ci/m6_jit_gcstep_guard.sh` -> `m6_jit_gcstep_guard`
  - `tools/ci/m6_jit_mcode_publish.sh` -> `m6_jit_mcode_publish`
  - `tools/ci/m6_jit_flush_hs.sh` -> `m6_jit_flush_hs`
  - `tools/ci/m6_jit.sh` -> `m6_jit`
- Twenty-first migrated scripts:
  - `tools/ci/m7_ffi_cdef_token.sh` -> `m7_ffi_cdef_token`
  - `tools/ci/m7_ffi_cdef_dup_stack.sh` -> `m7_ffi_cdef_dup_stack`
  - `tools/ci/m7_ffi_cparse_rollback.sh` -> `m7_ffi_cparse_rollback`
  - `tools/ci/m7_ffi_ctype_intern_l.sh` -> `m7_ffi_ctype_intern_l`
  - `tools/ci/m7_ffi_ctype_hash_publish.sh` -> `m7_ffi_ctype_hash_publish`
  - `tools/ci/m7_ffi_ctype_tab_retire.sh` -> `m7_ffi_ctype_tab_retire`
  - `tools/ci/m7_ffi_ctype_ticket_intern.sh` -> `m7_ffi_ctype_ticket_intern`
  - `tools/ci/m7_ffi_ctype_name_claim.sh` -> `m7_ffi_ctype_name_claim`
  - `tools/ci/m7_ffi_ctype_pointer_ids.sh` -> `m7_ffi_ctype_pointer_ids`
  - `tools/ci/m7_ffi_cdata_alloc.sh` -> `m7_ffi_cdata_alloc`
  - `tools/ci/m7_ffi_jit_cnew.sh` -> `m7_ffi_jit_cnew`
  - `tools/ci/m7_ffi_snap_restore_l.sh` -> `m7_ffi_snap_restore_l`
  - `tools/ci/m7_ffi_finreg.sh` -> `m7_ffi_finreg`
  - `tools/ci/m7_ffi_pin.sh` -> `m7_ffi_pin`
  - `tools/ci/m7_ffi_metatype.sh` -> `m7_ffi_metatype`
  - `tools/ci/m7_ffi_cdata_get_l.sh` -> `m7_ffi_cdata_get_l`
  - `tools/ci/m7_ffi_cdata_set_l.sh` -> `m7_ffi_cdata_set_l`
  - `tools/ci/m7_ffi_carith_l.sh` -> `m7_ffi_carith_l`
  - `tools/ci/m7_ffi_clib_cache.sh` -> `m7_ffi_clib_cache`
  - `tools/ci/m7_ffi_callback_install.sh` -> `m7_ffi_callback_install`
  - `tools/ci/m7_ffi_callback_runtime.sh` -> `m7_ffi_callback_runtime`
  - `tools/ci/m7_ffi_blocking.sh` -> `m7_ffi_blocking`
  - `tools/ci/m7_ffi.sh` -> `m7_ffi`
- Twenty-second migrated scripts:
  - `tools/ci/m8_weak.sh` -> `m8_weak`
- This first batch covers the main shapes the full migration needs:
  standalone C fixtures linked against selected runtime files, Lua tests under
  the built VM, C fixtures linked against `libluajit.a`, and source-order guard
  checks previously written in shell/awk/rg.
- The second batch adds repeated Lua child-process tests with environment
  defaults and an assertion-build marker guard. `ljtest.assert_not_match()`
  exists for exact shell-regex parity where a plain substring check would be
  weaker, e.g. rejecting `#if%s+LJ_MT`.
- The third batch adds reusable source range/block helpers and temporary-file
  helpers. These cover cleanup-before-STOPREQ ordering checks, VM safepoint
  marker checks, shutdown marker-file validation, and POSIX `os.*`
  reentrancy guards without shell `awk`/`grep`.
- The fourth batch turns the focused M2 arena shell scripts into launchers only.
  `ljtest.cc()` now accepts per-fixture compile flags for assertion fixtures,
  and `ljtest.assert_all_any_contains()` preserves `rg -F` style source-marker
  checks across multiple files.
- The fifth batch adds Lua source-scanning guard predicates for M5 publication,
  metamethod snapshot, jit attach/profile CAS, table/parser release-store, and
  per-TG tmpbuf checks. `ljtest.files()` provides deterministic source file
  enumeration for guard suites without shell `rg`/`awk` predicates.
- The sixth batch migrates the remaining non-TSan M4 C fixtures. These keep the
  original clean-build/link-against-`libluajit.a` behavior and preserve the C
  threading API shutdown marker guard in Lua.
- The seventh batch migrates small M5 runtime/table fixtures and their source
  guards. `ljtest.cc()` can now opt out of default flags and `-I src` for
  standalone models that intentionally do not link LuaJIT.
- The eighth batch migrates the M2 GC-header accessor guard plus focused M5
  runtime smoke wrappers for string.buffer publication, CType.name publication,
  and JIT table-store bridge coverage.
- The ninth batch migrates two focused M5 table hash-node fixtures while
  preserving the timeout-wrapped C fixture runs and source guard predicates for
  slot snapshots and KEYLOCK free-node reservation.
- The tenth batch migrates table allocation publication guardrails and the x64
  JIT HREF node-header hmask smoke/marker guard.
- The eleventh batch migrates stable hash-chain ordering, hash-vector
  publication, and hash-vector retirement fixtures with their source guards.
- The twelfth batch migrates the userdata type acquire/release publication
  guard, including constructor order checks and the Lua smoke test.
- The thirteenth batch migrates the HREFK recorder snapshot guard plus table
  node-header and FORWARD-value filtering fixtures.
- The fourteenth batch migrates the mutable table FLOAD guard and per-TG
  threading allocator routing smoke.
- The fifteenth batch migrates the string table CAS/rehash fixtures and
  publication source guards.
- The sixteenth batch migrates the M3 GC2/safepoint guards into Lua. The
  paranoia aggregate still preserves the existing stock-test assertion failure.
- The seventeenth batch migrates the focused x64 snapshot/publication guards,
  leaving the larger TSET nil-snapshot guard for a dedicated pass.
- The eighteenth batch migrates the benchmark/GC telemetry wrappers and the
  M9/M10 aggregate. The focused benchmark-regression wrapper remains the
  authoritative expensive CSV generation check.
- The nineteenth batch migrates the remaining focused M5 publication and
  local-cell/x64 TSET guards into a dedicated Lua suite.
- The twentieth batch migrates the M6 JIT scaffold wrappers and aggregate,
  keeping the aggregate as Lua orchestration over the focused cases.
- The twenty-first batch migrates the M7 FFI wrappers and aggregate.
- The twenty-second batch migrates the M8 weak/finalizer semantic gate.
- The twenty-third batch migrates the remaining M0 matrix,
  stock-suite runner, M4 TSan driver gate, and M5 aggregate wrapper. The only
  remaining shell entrypoint is the compatibility launcher for the Lua runner.
- Source-shape-only cases were removed from the runnable suite: M0 guardrails,
  M2 GC header accessor grep, the M5 source publication guards, M5 x64 upvalue
  publication, and M7 no-CTState-L. These should be replaced by behavior tests
  or C/Lua fixtures if the invariant is important.
- Shared suite helpers now live in `tests/lib/suite_utils.lua`. The migrated
  suites use that module for shell quoting, environment defaults, substring
  scans, plain occurrence counts, line iteration, identifier checks, list
  append, and common "no matching lines" source predicates instead of carrying
  per-suite copies.
- Added `m5_upvalue_publish_gc` as a behavior replacement for the deleted
  closed-upvalue publication source guards. It stores fresh GC objects through
  closed upvalues in interpreter, threaded, and hot JIT paths, forces GC/GC2,
  and proves the stored values remain reachable without scanning `src/`.
- Removed the remaining M4 threading source-marker checks. `m4_threading_api`,
  `m4_threading_capi`, and `m4_threading_shutdown` now rely on their Lua/C
  behavior fixtures instead of reading `src/lib_threading.c`, safepoint sources,
  or x64 VM text.
- Added shared `compile_luajit_c_fixture` and `run_luajit_c_fixture` methods to
  the Lua test harness, then moved the repeated M4, M5 fixture, and M5 table C
  fixture build/link/run boilerplate onto those helpers.
- Removed the `m2_arena_gcsweep` source-marker list. The case now relies on the
  runtime arena sweep C fixture itself, and the M2 LuaJIT-linked fixtures share
  the central C-fixture helper.
- Removed source-marker checks from the M5 sentinel, bytecode dump compatibility,
  registry root, and nomm-cache fixture cases. Those cases now rely on their C
  behavior fixtures instead of asserting implementation text in `src/`.
- Generated-output matching is still valid behavior coverage. JIT dumps,
  bytecode listings, benchmark output, and other artifacts produced by running
  the VM can keep targeted assertions; the cleanup target is direct inspection
  of implementation text under `src/`.
- Removed the remaining M5 fixture-suite source inspections from the string-table
  prep and CAS cases. The suite now relies on the string-table C fixtures for
  layout, marker-bit, resize/retire, duplicate-intern, and secondary-rehash
  behavior.
- Converted the M5 table suite to rely on its compiled C behavior fixtures
  instead of checking implementation markers in `src/` or fixture source text.
- Converted the M5 runtime suite to behavior-only smoke/regression tests,
  removing source-text guards around buffer, CType, JIT table, userdata,
  allocator, OS, and parser checks.
- Removed `m7_ffi_jit_cnew` source/assembly text guards while keeping its
  allocation stress, generated JIT dump checks, and stock FFI regression run.
- Converted `m3_safepoint_handshake` to rely on the compiled handshake
  fixture instead of checking safepoint/native-call implementation markers.
- Converted `m7_ffi_cparse_rollback` to behavior-only coverage through its C
  rollback fixture, Lua reader stress, anchor build, and cdef-token regression.
- Keep build-owning tests serial unless/until the Lua runner grows a shared
  build cache/lock. Existing shell gates often run `make clean`, so parallel
  migration validation can race `host/buildvm` or `libluajit.a` creation.
- Long-term runner case types should include marker guards, Lua subprocess
  tests, C fixtures, stock-suite aliases, benchmark gates, and aggregates.
  Profile-changing builds need named serial profiles such as default, assert,
  paranoia, no-JIT, ctype-anchor, and TSan.

Next good batches:

- M2 arena remaining scripts are low-risk because they mostly compile one C
  fixture plus a small fixed source set.
- M4 threading wrappers are mostly LuaJIT invocations after a clean build and
  are good candidates after adding per-test environment defaults.
- M5 table/string guard scripts are good once the framework has reusable
  marker helpers for "must contain", "must not contain", and ordered source
  snippets.
