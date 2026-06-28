# CI/Test Harness Audit - 2026-06-28

## Inventory

- `tools/ci` currently has 144 shell scripts.
- 138 scripts call `tools/ci/lua_test.sh`, so the Lua suite is already the real
  runner for most milestone cases.
- About 30 scripts still embed local `awk`/`grep` source guards before calling
  the Lua suite.
- The Lua suites/helpers had at least 45 explicit clean-build calls.

## Problems Found

1. Repeated clean builds in aggregate runs.
   Many migrated Lua cases call `clean_build(t)` even when they use the same
   default flags as the previous case. This slows aggregate runs and increases
   the chance of conflicting build artifacts when multiple CI commands run in
   the same checkout.

2. Shell compatibility wrappers are now mostly historical.
   Keeping script names is useful for milestone entry points, but the
   implementation should keep moving into `tests/suites/*.lua` and shared
   helpers instead of expanding shell guards.

3. Aggregate cases can bypass wrapper-only guards.
   `m7_ffi`, `m9_m10_gc`, and similar aggregate cases call Lua case names, not
   the shell wrappers. Any guard that only exists in `tools/ci/<case>.sh` is not
   covered by the aggregate path unless it is moved into the Lua suite or into a
   separately invoked static-lint entrypoint.

4. Source guards often pin exact helper names.
   Some guards are intentional tripwires, but broad string matches can block a
   better implementation that preserves the same safety invariant with a new
   helper boundary. New guards should prefer local helper-shape invariants and
   behavior tests over global textual bans.

5. Large shell guard files are hard to review.
   `m3_gc2_worker_scheduler.sh`, `m3_safepoint_handshake.sh`, `m7_ffi_finreg.sh`,
   and `m8_weak.sh` are large enough that future guard additions should usually
   go into Lua suite helper modules or focused shared shell helpers.

## Cleanup Landed In This Slice

- `tests/lib/ljtest.lua` now caches clean builds within one `tools/test.lua`
  process. The first clean build still runs, and changing `XCFLAGS` still forces
  a clean rebuild. Repeated same-flag clean builds skip the redundant `make
  clean && make`.
- Set `LJ_TEST_DISABLE_BUILD_CACHE=1` to recover the old always-clean behavior
  while debugging the harness itself.
- `tests/lib/suite_utils.lua` now gives generic `read_file()` the same
  source-file rejection used by `Test:read()` and result-file assertions.
  Deliberate static guards must call `read_source_file()` explicitly.
- Existing suite source guards in M3, M5 x64, M6 JIT, and local-cell helpers now
  use `read_source_file()`, making accidental future source reads visible.
- `add_luajit_c_fixture_cases()` now defaults to incremental builds instead of
  forced clean builds. Cases that need profile isolation can still pass
  `clean = true`.
- Removed the FFI C-call fresh-STOPREQ source guard from the M3 shell wrapper.
  The `m7_ffi_callback_runtime` C fixtures now cover the relevant behavior:
  native entry/leave restoration, nested callbacks, stale callback returns,
  callback blacklisting, and fresh STOPREQ delivery.
- Reduced `tools/ci/m7_ffi_callback_runtime.sh` to a thin compatibility
  launcher. The old helper-name and source-shape checks were blocking better
  implementation shapes without adding behavior coverage beyond the fixtures.

## Follow-up Landed Later On 2026-06-28

- `tools/ci/m4_threading_capi.sh` is now a thin compatibility launcher. Its
  join-result wait, mutex wait, and attach-order source guards moved to C API
  behavior coverage.
- `tools/ci/m4_threading_api.sh` now runs both `m4_threading_api` and
  `m4_threading_capi`, so the compatibility entry point still gates the old
  contracts without pinning `src/lib_threading.c` source shape.
- `tests/t-threading-capi.c` now covers fresh STOPREQ delivery for a join
  blocked on a done child with a busy owner and for a blocked
  `threading.mutex:lock()` call.
- `m4_threading_capi` has a `20s` timeout to turn future hangs into diagnostic
  failures.

## Next Refactors

1. Convert more FFI source guards from `tools/ci/m7_ffi_*.sh` into behavior
   fixtures where feasible. Only keep source guards for invariants that cannot
   be observed reliably through runtime behavior, such as low-level memory-order
   helper boundaries.
2. Replace pure compatibility shell bodies with a generated or shared launcher
   pattern while keeping their public filenames.
3. Split the largest GC/finalizer shell guards into suite-local helper modules
   so behavior tests and source guards live near the milestone cases they
   protect.
4. Revisit exact-name guards after each implementation slice; when a guard
   protects semantics rather than a required ABI, rewrite it around the semantic
   boundary before it blocks better code.
