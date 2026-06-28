# CI/Test Harness Audit - 2026-06-28

## Inventory

- `tools/ci` currently has 68 shell scripts.
- 62 scripts call `tools/ci/lua_test.sh` after doing real guard or orchestration
  work; zero pure alias wrappers remain.
- About 62 scripts still embed local `awk`/`grep`/`sed` source guards before
  calling the Lua suite.
- The Lua suites/helpers had at least 45 explicit clean-build calls.

## Problems Found

1. Repeated clean builds in aggregate runs.
   Many migrated Lua cases call `clean_build(t)` even when they use the same
   default flags as the previous case. This slows aggregate runs and increases
   the chance of conflicting build artifacts when multiple CI commands run in
   the same checkout.

2. Pure shell compatibility wrappers are unwanted legacy surface.
   Canonical execution is `tools/ci/lua_test.sh <case...>`. A `tools/ci/*.sh`
   file should exist only when it performs real lint, orchestration, or
   non-suite setup that has not yet moved into Lua.

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
- Removed the old helper-name and source-shape checks from
  `m7_ffi_callback_runtime`; the behavior fixture now carries that contract.

## Follow-up Landed Later On 2026-06-28

- The pure `m4_threading_capi` and `m4_threading_api` shell aliases were
  removed. Their join-result wait, mutex wait, and attach-order source guards
  moved to C API behavior coverage.
- `tests/t-threading-capi.c` now covers fresh STOPREQ delivery for a join
  blocked on a done child with a busy owner and for a blocked
  `threading.mutex:lock()` call.
- `m4_threading_capi` has a `20s` timeout to turn future hangs into diagnostic
  failures.
- The pure `m5_profile_stop_native` shell alias was removed. Its old
  profiler-stop helper-name/source-order guards moved to
  `tests/t-profile-stop-native.c` behavior assertions for sticky STOPREQ
  cleanup, fresh STOPREQ during native timer-thread join, callback-error
  containment, busy callback coroutine ownership, and registry-anchor cleanup
  before interrupted `profile.stop()` unwinds.
- Removed 76 pure shell aliases in this pass. Use
  `tools/ci/lua_test.sh <case...>` for those cases.
- Slimmed `tools/ci/m7_ffi_blocking.sh` by removing source-shape checks for
  duplicate blacklist reconciliation and default recorder abort behavior; those
  contracts are covered by `t-ffi-cbblack-race`, `t-ffi-blocking.lua`, and the
  recorder-off fixture. The remaining checks enforce non-observable CType/load
  boundaries and the compile-time traced-FFI fence.
- Removed old bytecode-dump compatibility support for pre-lockless dump
  versions. The loader now accepts only the current lockless bytecode version,
  the `proto_legacyuv` path was deleted, and the former compatibility fixture is
  now `m5_bcdump_current`, focused on current-format validation and malformed
  current dump rejection.
- Removed unused/public C header compatibility aliases for `luaL_putchar`,
  `lua_strlen`, `lua_open`, `lua_getregistry`, `lua_getgccount`,
  `lua_Chunkreader`, and `lua_Chunkwriter`; repo-internal call sites now use
  the canonical APIs directly.

Verification for the alias removal:

- `tools/ci/lua_test.sh --list`
- `tools/ci/lua_test.sh m0_source_guard`
- `tools/ci/lua_test.sh m5_profile_stop_native`
- `tools/ci/lua_test.sh m4_threading_api m4_threading_capi`
- `tools/ci/lua_test.sh m7_ffi_blocking m7_ffi_callback_runtime`
- `tools/ci/lua_test.sh m5_bcdump_current`
- `git diff --check`

## Next Refactors

1. Convert more FFI source guards from `tools/ci/m7_ffi_*.sh` into behavior
   fixtures where feasible. Only keep source guards for invariants that cannot
   be observed reliably through runtime behavior, such as low-level memory-order
   helper boundaries.
2. Do not add new pure shell aliases. If a case is fully Lua-owned, run it
   through `tools/ci/lua_test.sh <case...>`.
3. Split the largest GC/finalizer shell guards into suite-local helper modules
   so behavior tests and source guards live near the milestone cases they
   protect.
4. Revisit exact-name guards after each implementation slice; when a guard
   protects semantics rather than a required ABI, rewrite it around the semantic
   boundary before it blocks better code.
