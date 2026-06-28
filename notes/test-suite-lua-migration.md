# Lua test-suite migration

## 2026-06-28 source-search guard removal

- Removed the old source-file content guard API and generic `assert_file_*`
  compatibility wrappers from the Lua test harness.
- Removed the old `suite_runtime` build/C-fixture compatibility exports; suites
  now import `suite_build` directly for those helpers.
- Kept generated dump assertions because JIT/bytecode/ASM dump text is a
  generated behavior surface, not repository source.
- Switched output-file checks to read the captured output and assert text
  directly.
- Source-search policy now lives in `notes/ci-source-search-policy.md`.

Validation:

- `tools/ci/lua_test.sh m3_vmevent_native_stdio m4_threading_shutdown m8_weak`
- `git diff --check`

## 2026-06-20

- Audited active `tests/suites` and `tools/ci` Lua tests for source-content
  assertions. No active test was found that reads `src/` and checks source text.
- Runtime output, bytecode output, and JIT dump matching are kept as behavior
  checks.
- Moved shared JIT dump parsing helpers from `m6_jit.lua` into
  `tests/lib/suite_jit.lua`.
- Moved the M7 IR dump probe helper into `suite_jit.lua`.
- Centralized generic text assertions in `suite_utils.lua` and removed unused
  path-based text assertion methods from `ljtest.lua`.

Validation:

- `tools/ci/lua_test.sh m6_jit_barrier_xpoll m6_jit_xbar_xpoll m6_jit_aref_pair_guard m6_jit_table_store_helper`
- `tools/ci/lua_test.sh m7_ffi_finreg m7_ffi_jit_cnew`

## 2026-06-20 runtime/helper cleanup

- Centralized LuaJIT script/code runners in `tests/lib/suite_runtime.lua`,
  including `joff`/`jon` handling and build-owning wrappers.
- Centralized C fixture orchestration in `suite_runtime`; `ljtest.lua` now keeps
  primitive `run`, `make`, `build`, `cc`, and `luajit` operations only.
- Shared fixture library defaults via `suite_utils.luajit_fixture_libs`.
- Replaced benchmark `rg` shell-pipeline checks with Lua-side command capture
  and text assertions.
- Shared top-level and aggregate case dispatch through `suite_utils.run_case`.
- Routed suite-level LuaJIT invocations through runtime helpers. Direct
  `t:luajit` calls now remain only in `suite_runtime` and the base harness.

Validation:

- `tools/ci/lua_test.sh m9_gc_stats m10_generational`
- `tools/ci/lua_test.sh m8_weak`
- `tools/ci/lua_test.sh m7_ffi_cparse_rollback`
- `tools/ci/lua_test.sh m2_arena_bitmap m2_arena_state m4_thr_substrate m5_nbtab_model m5_strtab_cas m5_x64_tget_array_header m5_x64_table_next_snapshot m5_x64_tset_nil_snapshot`
- `tools/ci/lua_test.sh m4_tsan_drivers`
- `tools/ci/lua_test.sh m9_bench_smoke m9_bench_regression`
- `tools/ci/lua_test.sh m9_m10_gc`
- `tools/ci/lua_test.sh run_stock_tests -- src/luajit --quiet lang/andor.lua`
- `tools/ci/lua_test.sh m2_arena_all`
- `tools/ci/lua_test.sh m5_buffer_publish m5_ctype_name_publish m5_jit_hash_store_nyi m5_x64_getmetatable_node_order m5_x64_tget_array_header m5_threading_alloc m5_os_reentrant m5_parser_capture_meta`
- `tools/ci/lua_test.sh m5_jit_table_fload_mutable m5_jit_href_node_order m5_jit_hrefk_record_snapshot m5_udtype_publish m5_math_random_tg`
- `tools/ci/lua_test.sh m3_gc2_worker_scheduler m5_upvalue_publish_gc m7_ffi_callback_runtime m8_weak m9_bench_smoke m0_matrix`

## 2026-06-20 registry hardening

- Added a duplicate-name assertion to `tests/suites/init.lua` so a new suite
  case cannot silently overwrite an existing Lua test registration.
- Kept this as a framework invariant; it does not inspect source files.

Validation:

- `tools/ci/lua_test.sh --list`
- `tools/ci/lua_test.sh run_stock_tests -- src/luajit --quiet lang/andor.lua`

## 2026-06-20 assertion module split

- Moved shared text, file-result, and dump-result assertions into
  `tests/lib/suite_assert.lua`.
- Left compatibility exports in `suite_utils.lua` while routing dump and
  file-result suites to `suite_assert` directly.
- Kept the source-file content guard in the assertion module.
- Added the same source-file content guard to raw `Test:read()` so tests cannot
  bypass it by reading `src/` or top-level C fixture sources directly.
- Confirmed JIT/bytecode dump matching remains supported as result matching.

Validation:

- `tools/ci/lua_test.sh --list`
- Direct `suite_assert`/`ljtest` smoke checks for source-read rejection and dump
  result matching.
- `tools/ci/lua_test.sh m4_threading_shutdown m6_jit_cell_ops m7_ffi_jit_cnew`

## 2026-06-28 explicit source-read API

- Added the same source-file rejection to `tests/lib/suite_utils.lua`
  `read_file()` so runnable suites cannot bypass the `Test:read()` guard.
- Added `suite_utils.read_source_file()` for deliberate static source guards.
  Current M3/M5/M6/local-cell source guards use this explicit API.
- Changed `add_luajit_c_fixture_cases()` to default to incremental builds;
  cases that require a separate build profile must opt into `clean = true`.
- `tests/lib/ljtest.lua` caches repeated same-flag clean builds within one
  `tools/test.lua` process. Set `LJ_TEST_DISABLE_BUILD_CACHE=1` for the old
  always-clean behavior while debugging the harness.
- Removed source-shape guards from `m7_ffi_callback_runtime.sh` and kept the
  public wrapper as a thin launcher. The callback runtime case now relies on
  its C/Lua behavior fixtures for native-state restoration, stale callback
  returns, callback blacklisting, and fresh STOPREQ behavior.
- Removed the M3 shell wrapper's FFI C-call source guard because the same
  native-entry STOPREQ path is now covered by the M7 callback STOPREQ fixture.

Validation:

- `tools/ci/lua_test.sh --list`
- `tools/ci/lua_test.sh m7_ffi_ccall_native m7_ffi_callback_runtime`
- `tools/ci/m3_safepoint_handshake.sh`

## 2026-06-20 CI wrapper parity

- Historical note: this pass added thin shell wrappers for Lua cases that were
  missing shell launchers. A later cleanup removed pure aliases; canonical
  execution is now `tools/ci/lua_test.sh <case...>`.
- Simplified `tools/ci/run_stock_tests.sh` so Lua owns default-bin and stock
  argument handling.
- Rechecked wrapper parity; all Lua test cases now have matching wrappers
  except the `lua_test.sh` runner itself, which is intentionally not a case.

Validation:

- `comm -23 <(tools/ci/lua_test.sh --list | sort) <(find tools/ci -maxdepth 1 -name '*.sh' -printf '%f\n' | sed 's/[.]sh$//' | sort)`
- `tools/ci/run_stock_tests.sh src/luajit --quiet lang/andor.lua`
- `tools/ci/m5_upvalue_publish_gc.sh`
- `tools/ci/m6_jit_perftools_native.sh`
- `tools/ci/m7_ffi_clib_ldscript.sh`
- `tools/ci/m7_ffi_nested_state.sh`

## 2026-06-20 stock selector cleanup

- Replaced brittle numeric stock-test selectors in `m7_ffi_jit_cnew` with stable
  stock file roots.
- Routed the paranoia stock checks through `suite_runtime.run_stock`, removing
  the suite-local shell assembly.

Validation:

- `tools/ci/lua_test.sh --list`
- `tools/ci/lua_test.sh m7_ffi_jit_cnew`

## 2026-06-20 assertion public surface

- Removed transitional assertion re-exports from `suite_utils.lua`.
- `suite_assert.lua` is now the public module for text/file/dump assertions and
  source-content path guards.
- `suite_utils.lua` still uses `suite_assert` internally for command-output
  assertions.

Validation:

- `tools/ci/lua_test.sh --list`
- Direct `ljtest` smoke check for source-read rejection and normal file reads.
- `make -C src clean && tools/ci/lua_test.sh m9_bench_regression`
- `tools/ci/lua_test.sh m4_threading_shutdown m6_jit_cell_ops`

## 2026-06-20 M9 build isolation

- M9/M10 suite entry points now request a clean default build before running.
- This prevents benchmark and GC telemetry cases from inheriting prior
  `XCFLAGS` state, such as the paranoia build used by M7 FFI JIT CNEW checks.

Validation:

- `tools/ci/lua_test.sh --list`
- `tools/ci/lua_test.sh m7_ffi_jit_cnew m9_bench_regression`

## 2026-06-20 aggregate dependency validation

- Added deferred `deps` validation to `tests/suites/init.lua`.
- Aggregate cases now declare their child case lists so `--list` catches
  misspelled or removed child tests even when children are registered by later
  suite modules.
- Added dependency metadata for M2, M3, M5, M6, M7, and M9/M10 aggregates.

Validation:

- `tools/ci/lua_test.sh --list`
- Direct Lua metadata smoke over aggregate `deps` entries.
- `tools/ci/lua_test.sh m2_arena_all`

## 2026-06-20 source guard and aggregate execution tightening

- Broadened source-content guards so behavior assertions cannot read test,
  wrapper, tool, aux, bench, or `src/` source files directly.
- Fixed `run_stock_tests` passthrough parsing so options like `--quiet` are not
  mistaken for an explicit LuaJIT binary, while explicit binaries still work.
- Made `m9_m10_gc` run its declared dependency list as the single source of
  truth for aggregate execution.
- Added bounded command capture and narrowed the benchmark-regression case to a
  generated mini benchmark for `run_baseline.sh`/CSV behavior. The full
  benchmark workload is still available separately; during validation,
  `BENCH_SCALE=0.001 src/luajit aux/bench/bench.lua closures_upval` hung under
  JIT while `-joff` completed quickly.

Validation:

- `tools/ci/lua_test.sh --list`
- `tools/ci/run_stock_tests.sh --quiet lang/andor.lua`
- `tools/ci/run_stock_tests.sh src/luajit --quiet lang/andor.lua`
- Direct `suite_assert`/`ljtest` smoke check for broader source-read rejection
  and result-file matching.
- `tools/ci/lua_test.sh m9_bench_regression`
- `tools/ci/lua_test.sh m9_m10_gc`
- `git diff --check`

## 2026-06-20 x64 VM array TSET reroute

- Added `lj_tab_storetv_forvm_array()` for the x64 interpreter array TSET fast
  paths. It CAS-stores and re-resolves the logical array slot if the original
  slot is forwarded/retired between the VM fast-path check and the write.
- Kept barriers outside the helper; the existing x64 VM post-store table
  barrier path runs on the returned slot.
- Routed only the proven single-slot array fast paths (`TSETV`, `TSETB`,
  `TSETR`) through the helper. The shared `BC_TSETR_Z` fallback remains on
  plain resolved-slot store semantics because it can be reached after
  `lj_tab_setinth()`.
- Left initial observed `FORWARD` on the existing fallback path to preserve the
  current nil/`__newindex` decision point.
- Extended `t-x64-tset-forward.c` with a deterministic stale old-array helper
  check: a retiring old array plus nextgen must write to the new array slot.

Validation:

- `make -C src clean && make -C src -j2`
- `tools/ci/lua_test.sh m5_x64_tset_nil_snapshot m5_tab_value_publish m6_jit_table_store_helper`
- `git diff --check`

## 2026-06-20 source guard behavior case

- Added `m0_source_guard` as a first-class Lua suite case.
- The case verifies that framework reads/assertions reject source paths while
  generated result-file matching still works.
- Added the thin CI compatibility wrapper and rechecked wrapper parity.

Validation:

- `tools/ci/lua_test.sh --list`
- `tools/ci/lua_test.sh m0_source_guard`
- `tools/ci/m0_source_guard.sh`
- `comm -23 <(tools/ci/lua_test.sh --list | sort) <(find tools/ci -maxdepth 1 -name '*.sh' -printf '%f\n' | sed 's/[.]sh$//' | sort)`
- `git diff --check`

## 2026-06-20 Lua benchmark CSV gate

- Added `tests/lib/bench_csv.lua` for benchmark CSV parsing, benchmark text
  conversion, geomean comparison, and structured failure reporting.
- Moved `m9_bench_regression` off `bench/compare_baseline.sh` and
  `bench/run_baseline.sh`; it now validates pass, geomean fail, missing row,
  extra row, non-positive value, and generated mini benchmark conversion
  directly in Lua.

Validation:

- `tools/ci/lua_test.sh --list`
- `tools/ci/lua_test.sh m9_bench_regression`
- `tools/ci/lua_test.sh m9_m10_gc`
- `git diff --check`

## 2026-06-20 shared thread harness helpers

- Added `thread_harness.env_number()` for shared environment numeric defaults.
- Added `thread_harness.assert_stack_grows()` for worker-thread stack-growth
  probes used by FFI stress cases.
- Replaced duplicated stack-recursion and environment parsing helpers in M4,
  M5, M6, M7, and M8 Lua behavior tests.
- `m8_weak` aggregate is currently blocked by the unrelated
  `t-gc2-traverse_m8` C fixture hang; a direct bounded run returned 124.

Follow-up on 2026-06-28:

- `m8_weak` now passes through the Lua suite runner. The M8 C fixture batches
  have per-fixture 20s timeouts so any future `t-gc2-traverse_m8` regression
  fails bounded instead of hanging the aggregate.

Validation:

- `timeout 240s tools/ci/lua_test.sh m4_threading_litmus m4_threading_stress m5_os_reentrant m6_jit_mcode_publish m7_ffi_clib_cache m7_ffi_callback_runtime m7_ffi_carith_l m7_ffi_ctype_intern_l m7_ffi_finreg m7_ffi_metatype`
- Direct `t-weak-modes.lua` and `t-ffi-gc-finreg.lua` runs under JIT-off and
  JIT-on with `LUA_PATH=tests/lib/?.lua;src/?.lua;src/jit/?.lua;;`.
- `tools/ci/lua_test.sh --list`
- Duplicate helper scan for removed local `grow_stack` and repeated numeric
  environment parsing.
- `git diff --check`

- `tools/ci/lua_test.sh --list`
- `tools/ci/lua_test.sh m9_bench_regression`
- `tools/ci/lua_test.sh m9_m10_gc`
- `git diff --check`

## 2026-06-20 shared JIT trace-count helper

- Added `tests/lib/jit_harness.lua` with a non-tracing `trace_count(limit)`.
- Migrated duplicate trace-count helpers in standalone FFI/JIT Lua tests and
  M5 embedded Lua snippets.
- Updated `m6_jit_token` to launch `t-jit-secondary.lua` with the test library
  path, because it now imports shared helpers.

Validation:

- `tools/ci/lua_test.sh m5_jit_hash_store_nyi m5_jit_trace_publish m6_jit_token m7_ffi_ccall_native m7_ffi_finreg m7_ffi_jit_cnew`
- `tools/ci/lua_test.sh --list`
- duplicate trace-count helper `rg` check over migrated files
- `git diff --check`

## 2026-06-20 shared C table-forward helpers

- Added `tests/lib/tab_forward_helpers.h` for C fixture helpers around
  `FORWARD` slot writes, integer TValue assertions, string/number/hash slot
  lookup, and Lua chunk load/run wrappers.
- Migrated the table-forwarding C fixtures to use the shared primitive helpers:
  `t-tab-cas-store.c`, `t-tab-forward-filter.c`, `t-jit-forward-store.c`, and
  the x64 forwarding fixtures.
- Kept full forwarding setup blocks local because forwarded/current-retiring
  generation state differs intentionally between behavior cases.

Validation:

- `timeout 360s tools/ci/lua_test.sh m5_tab_forward_filter m5_tab_cas_store m5_tab_value_publish m5_x64_tset_nil_snapshot m5_x64_tget_array_header m5_x64_tgets_node_order m5_x64_ipairs_snapshot m5_x64_itern_snapshot m5_x64_table_next_snapshot m6_jit_table_store_helper`
- `tools/ci/lua_test.sh --list`
- duplicate helper scan over migrated C fixtures
- `git diff --check`

## 2026-06-20 stock runner cwd plumbing

- Added `cwd` support to the Lua test harness `Test:run()`.
- Routed stock-suite execution through `Test:run()` argv/env/cwd fields instead
  of assembling a `cd ... && ...` command inside `suite_runtime.run_stock()`.
- Behavior remains owned by the Lua runner; pure shell aliases have since been
  removed.

Validation:

- `tools/ci/lua_test.sh run_stock_tests -- --quiet lang/andor.lua`
- `timeout 240s tools/ci/lua_test.sh m5_upvalue_publish_gc m7_ffi_jit_cnew`
- `tools/ci/run_stock_tests.sh src/luajit --quiet lang/andor.lua`
- `tools/ci/lua_test.sh --list`
- `git diff --check`

## 2026-06-20 Lua-owned benchmark scripts

- Added `bench/bench_csv_cli.lua` as a thin CLI around `tests/lib/bench_csv.lua`.
- Routed `bench/compare_baseline.sh` CSV comparison through the Lua benchmark
  module instead of AWK.
- Routed `bench/run_baseline.sh` benchmark text-to-CSV conversion through the
  Lua module.
- Routed `aux/bench/run.sh` baseline/compare CSV and geomean handling through
  the same Lua CLI, while leaving benchmark process launch and scaling loops in
  shell.
- Extended `m9_bench_regression` to exercise the Lua CLI plus shell entrypoints
  with generated benchmark output. Generated output and CLI result matching are
  behavior tests.

Validation:

- `timeout 180s tools/ci/lua_test.sh m9_bench_regression`
- `tools/ci/lua_test.sh m9_bench_smoke`
- `BENCH_SCALE=0.0001 BENCH_FILTER=arith_loop aux/bench/run.sh baseline src/luajit`
- `tools/ci/lua_test.sh --list`
- `git diff --check`

## 2026-06-20 shared C Lua fixture helpers

- Added `tests/lib/lua_fixture_helpers.h` for C fixtures that need a Lua state
  with open libraries and fail-fast Lua chunk execution.
- Migrated low-risk JIT/FFI C fixtures that shared the same `dostring`
  boilerplate: recorder token, trace vector/mcode retirement, cparser rollback,
  ctype duplicate-name, ctype table retirement, and ctype ticket interning.
- Left fixtures with custom negative-error checks or specialized call protocol
  local for now.

Validation:

- `timeout 240s tools/ci/lua_test.sh m6_jit_token m5_jit_trace_publish m7_ffi_cparse_rollback m7_ffi_ctype_name_claim m7_ffi_ctype_tab_retire m7_ffi_ctype_ticket_intern`
- `tools/ci/lua_test.sh --list`
- duplicate local Lua helper scan over migrated C fixtures
- `git diff --check`

## 2026-06-20 shared Lua GC and arg helpers

- Reused `thread_harness.fullgc()` in weak-table and FFI pinning behavior
  tests instead of carrying local full-GC loops.
- Routed `t-threading-alloc.lua` numeric arguments through
  `thread_harness.arg_number()` so environment overrides and CLI defaults are
  handled consistently with the other threading tests.

Validation:

- `tools/ci/lua_test.sh m5_threading_alloc m7_ffi_pin`
- Direct `t-weak-modes.lua` runs under JIT-off and JIT-on with
  `LUA_PATH=tests/lib/?.lua;src/?.lua;src/jit/?.lua;;`.
- `tools/ci/lua_test.sh --list`
- Duplicate helper scan for local `fullgc` and direct `arg` numeric parsing.
- `git diff --check`

## 2026-06-20 structured LuaJIT result capture

- Added stdout/stderr redirection options to `Test:run()`.
- Routed LuaJIT dump and output-capture helpers through argv/env/timeout
  runner fields instead of local shell-string assembly.
- Kept generated bytecode/JIT dump matching as behavior result matching.

Validation:

- `timeout 360s tools/ci/lua_test.sh m5_cell_ops m6_jit_cell_ops m6_jit_barrier_xpoll m6_jit_xbar_xpoll m6_jit_aref_pair_guard m6_jit_hrefk_nodehdr m7_ffi_finreg`
- `tools/ci/lua_test.sh --list`
- `git diff --check`

## 2026-06-20 shared C Lua status helper

- Added `ljt_lua_assert_ok()` to `tests/lib/lua_fixture_helpers.h`.
- Reused the helper in current bytecode validation, NaN TValue tag, nomm cache, and
  lua_State owner fixtures.
- Left expected-error and fixture-specific owner checks local.

Validation:

- `timeout 240s tools/ci/lua_test.sh m5_bcdump_current m5_itype_nan m5_nomm_cache m5_state_owner`
- `tools/ci/lua_test.sh --list`
- Duplicate local status-helper scan over migrated fixtures.
- `git diff --check`

## 2026-06-20 shared table fixture primitives

- Added table-forward fixture helpers for SID-bucket string generation,
  string-key integer stores/assertions, C-string convenience wrappers, and
  visible `lj_tab_next()` entry counting.
- Migrated table chain-order, KEYLOCK lookup, slot-snapshot, and FORWARD
  filter fixtures to those primitives.
- Kept fixture-specific KEYLOCK/FORWARD setup and table-retirement scenarios
  local.

Validation:

- `timeout 300s tools/ci/lua_test.sh m5_tab_chain_order m5_tab_keylock_lookup m5_tab_slot_snapshot m5_tab_forward_filter`
- `tools/ci/lua_test.sh --list`
- Duplicate local table-helper scan over migrated fixtures.
- `git diff --check`

## 2026-06-20 Lua benchmark driver

- Added `tests/lib/bench_driver.lua` for benchmark process capture, baseline
  CSV generation, aux baseline files, binary comparison, and scaling output.
- Added Lua CLI commands for benchmark driver operations, including `aux-run`
  mode dispatch.
- Reduced `bench/run_baseline.sh` and `aux/bench/run.sh` to compatibility
  launchers around the Lua driver.
- Extended M9 regression coverage to exercise the direct driver and new CLI
  commands; generated result matching remains behavior testing.

Validation:

- `timeout 240s tools/ci/lua_test.sh m9_bench_regression m9_bench_smoke`
- `BENCH_SCALE=0.0001 BENCH_FILTER=arith_loop aux/bench/run.sh baseline src/luajit`
- `tools/ci/lua_test.sh --list`
- `git diff --check`

## 2026-06-20 shared C Lua chunk helpers

- Reused `lua_fixture_helpers.h` chunk helpers in FFI callback carrier/runtime
  fixtures, GC2 hard-check fixtures, GC2 paranoia, and mcode W^X protection.
- Kept fixture-specific callback, owner, blacklist, paranoia-finalizer, and
  GC state assertions local.

Validation:

- `timeout 360s tools/ci/lua_test.sh m7_ffi_callback_install m7_ffi_callback_runtime m6_jit_perftools_native m6_jit_gc2_readiness m3_gc2_paranoia`
  - Passed through the migrated callback fixtures, `t-jit-perftools-native`,
    `t-gc2-jit-hard-check`, and `t-gc2-paranoia`.
  - Timed out in `t-gc2-traverse`, after the migrated `t-gc2-paranoia` fixture
    passed.
- `timeout 240s tools/ci/lua_test.sh m6_jit_alloc_account m6_jit_mcode_publish`
  - Passed `t-gc2-interp-hard-check` and `t-jit-mcode-prot`.
  - Failed later in `t-jit-mcode-fresh.lua` with an existing memory-leak
    assertion.
- Direct focused compile/run of `t-gc2-paranoia.c` with paranoia flags and
  `t-jit-mcode-prot.c`.
- `tools/ci/lua_test.sh --list`
- Duplicate local chunk-helper scan over migrated fixtures.
- `git diff --check`
