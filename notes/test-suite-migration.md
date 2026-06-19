Lua test-suite migration notes:

- Added `tools/test.lua` plus `tests/lib/ljtest.lua` as the Lua-owned test
  runner/framework. The framework owns command execution, VM builds, C fixture
  compilation, LuaJIT invocation, and source guard assertions.
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
- Fifth migrated scripts:
  - `tools/ci/m5_gcroot_publish.sh` -> `m5_gcroot_publish`
  - `tools/ci/m5_meta_snapshot.sh` -> `m5_meta_snapshot`
  - `tools/ci/m5_jit_attach_publish.sh` -> `m5_jit_attach_publish`
  - `tools/ci/m5_jit_profile_publish.sh` -> `m5_jit_profile_publish`
  - `tools/ci/m5_table_parser_publish.sh` -> `m5_table_parser_publish`
  - `tools/ci/m5_tmpbuf_tg.sh` -> `m5_tmpbuf_tg`
- Sixth migrated scripts:
  - `tools/ci/m4_thr_substrate.sh` -> `m4_thr_substrate`
  - `tools/ci/m4_chan_stress.sh` -> `m4_chan_stress`
  - `tools/ci/m4_threading_capi.sh` -> `m4_threading_capi`
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
