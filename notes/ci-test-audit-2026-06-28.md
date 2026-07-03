# CI/Test Harness Audit - 2026-06-28

Historical note: this audit predates the current invariant-testing model. The
current guidance is `notes/ci-invariant-testing.md`: CI and tests prove
behavior or generated artifacts; implementation-only rules are documented next
to the constrained code.

## Inventory

- `tools/ci` currently has 66 shell scripts.
- Most scripts call `tools/ci/lua_test.sh` after doing real validation or
  orchestration work; zero pure alias wrappers remain.
- At the time of this audit, about 60 scripts still embedded local
  shell legacy wrappers before calling the Lua
  suite. Those have since been removed or replaced with behavior fixtures,
  generated-artifact checks, and documentation.
- `tools/ci/lua_test.sh --list` exposes 100+ named Lua-suite cases.
- The Lua suites/helpers still contain many explicit clean-build calls.

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

3. Aggregate cases can bypass wrapper-only checks.
   `m7_ffi`, `m9_m10_gc`, and similar aggregate cases call Lua case names, not
   the shell wrappers. Any guard that only exists in `tools/ci/<case>.sh` is not
   covered by the aggregate path unless it is moved into the Lua suite or into a
   separately invoked static-lint entrypoint.

4. Repository legacy wrappers often pinned exact helper names.
   Broad string matches blocked better implementations that preserved the same
   safety invariant with a new helper boundary. New tests must use behavior,
   C fixtures, generated dump/ASM checks, or documentation instead of
   helper comments.

5. Large shell wrapper files are hard to review.
   `m3_gc2_worker_scheduler.sh`, `m3_safepoint_handshake.sh`, `m7_ffi_finreg.sh`,
   and `m8_weak.sh` are large enough that future legacy wrappers should usually
   go into Lua suite helper modules or focused shared shell helpers.

## Cleanup Landed In This Slice

- `tests/lib/ljtest.lua` now caches clean builds within one `tools/test.lua`
  process. The first clean build still runs, and changing `XCFLAGS` still forces
  a clean rebuild. Repeated same-flag clean builds skip the redundant `make
  clean && make`.
- Set `LJ_TEST_DISABLE_BUILD_CACHE=1` to recover the old always-clean behavior
  while debugging the harness itself.
- `tests/lib/suite_utils.lua` temporarily gave generic `read_file()` the same
  path-based source access behavior used by `Test:read()` and result-file
  assertions. The later cleanup removed that behavior, the deliberate
  source-reading helper, and eventually all path-based source access special
  cases. Current coverage is documentation plus behavior/generated-artifact
  tests, not harness path-based checks.
- `add_luajit_c_fixture_cases()` now defaults to incremental builds instead of
  forced clean builds. Cases that need profile isolation can still pass
  `clean = true`.
- Removed the FFI C-call fresh-STOPREQ legacy wrapper from the M3 shell wrapper.
  The `m7_ffi_callback_runtime` C fixtures now cover the relevant behavior:
  native entry/leave restoration, nested callbacks, stale callback returns,
  callback blacklisting, and fresh STOPREQ delivery.
- Removed the old helper-name and implementation-shape notes from
  `m7_ffi_callback_runtime`; the behavior fixture now carries that contract.

## Follow-up Landed Later On 2026-06-28

- The pure `m4_threading_capi` and `m4_threading_api` shell aliases were
  removed. Their join-result wait, mutex wait, and attach-order legacy wrappers
  moved to C API behavior coverage.
- `tests/t-threading-capi.c` now covers fresh STOPREQ delivery for a join
  blocked on a done child with a busy owner and for a blocked
  `threading.mutex:lock()` call.
- `m4_threading_capi` has a `20s` timeout to turn future hangs into diagnostic
  failures.
- The pure `m5_profile_stop_native` shell alias was removed. Its old
  profiler-stop helper-name/order assertions moved to
  `tests/t-profile-stop-native.c` behavior assertions for sticky STOPREQ
  cleanup, fresh STOPREQ during native timer-thread join, callback-error
  containment, busy callback coroutine ownership, and registry-anchor cleanup
  before interrupted `profile.stop()` unwinds.
- Removed 76 pure shell aliases in this pass. Use
  `tools/ci/lua_test.sh <case...>` for those cases.
- The old `m7_ffi_blocking` wrapper was folded into the Lua-owned
  `m7_ffi_ccall_native` behavior case. Duplicate callback-blacklist
  reconciliation, absence of the removed `ffi.blocking` public marker, default
  recorder abort behavior, and native-state blocking progress are covered by
  `tests/t-ffi-cbblack-race.c`, `tests/t-ffi-ccall-native.lua`, and the
  recorder-off fixture rather than implementation-shape notes.
- Follow-up stock-behavior correction: bytecode-dump compatibility support for
  stock v2 and transitional v3 dumps was restored. That path is not a
  threading-only legacy entrypoint; it affects `load`, `luaL_loadbuffer*`, and
  precompiled chunk interoperability. The active `m5_bcdump_compat` fixture
  mutates generated dumps to verify v2/v3 loading while still rejecting
  lockless-only cell opcodes in old dump versions.
- Reverted the removal of stock LuaJIT C header aliases such as
  `luaL_putchar`, `lua_strlen`, `lua_open`, `lua_getregistry`,
  `lua_getgccount`, `lua_Chunkreader`, and `lua_Chunkwriter`; these are stock
  LuaJIT public API compatibility macros and should remain available.
- Moved `t-ffi-finreg-free-invariant.c` into the `m7_ffi_finreg` Lua-suite
  case and removed the ad hoc hardcoded `/tmp` compile from
  `tools/ci/m7_ffi_finreg.sh`.
- Removed the exact parser-token STOPREQ helper-spelling rule from
  `m7_ffi_cdef_token`; `t-ffi-cdef-token-stopreq.c` now owns the behavior
  contract for sticky STOPREQ cleanup and fresh STOPREQ while parked.
- Converted the element-size parser-lock-fallback expectation into active-token
  behavior coverage in `t-ffi-element-size-snapshot.c`.
- Converted the cdata field parser-lock-fallback expectation into active-token
  behavior coverage in `t-ffi-field-snapshot.c`, including direct fields,
  pointer auto-deref, misses, metatype dispatch, and constructor constants.
- Extended `t-ffi-cparse-rollback-reader.lua` so failed cdefs cannot leak
  constructor constants or constructor fields.
- Added narrow field-helper guards in `m7_ffi_typeinfo_snapshot.sh` for the
  remaining non-observable implementation shape: ID-rooted field waits, no
  parser-lock acquisition in `lj_cdata_index_l()`, and no sequence-free field
  snapshot misses.

## Legacy/Compat Audit Findings

- Highest-priority legacy cleanup is in GC2 bridge scaffolding, not FFI pointer
  compatibility. Exact-name assertions around `legacy_*` sweep/finalizer/weak
  helpers should become semantic behavior tests before the names are removed.
- The old internal M10 legacy mark-suppression helper name was removed in favor
  of `lj_gc2_minor_roots_skip_bridge_mark()`, which describes the actual
  policy: minor-root cycles skip only the arena-to-GC2 bridge mark, not legacy
  marking itself.
- Duplicate legacy wrappers in weak/worker CI were a
  temporary migration problem; exact-name repository assertions should be
  deleted, not collapsed, once behavior fixtures or documentation own the
  contract.
- The duplicate M8 sweep/finalizer assertion block was removed, while M8 still
  owns weak/finalizer behavior fixtures.
- The internal sweep-close bridge helper was renamed to
  `lj_gc2_sweep_bridge_close()`, keeping the behavior boundary while removing
  another exact legacy helper name from production source.
- The GC2 sweep close-readiness latch/helper surface was renamed to
  `sweep_bridge_ready` / `lj_gc2_sweep_bridge_*`, and the scheduler guard now
  checks the current bridge ownership surface without carrying old-name
  tombstones for the removed `sweep_legacy_ready` names.
- Removed more duplicate or tombstone-only legacy wrappers: M3 no longer duplicates the
  x64 `barrierback` check owned by M5, M10 no longer duplicates the stats-table
  checks owned by M9, and M10/M3 no longer carry old-helper-name tombstones for
  already-removed mark/sweep bridge wrappers.
- Migrated weak completion telemetry and tests to `weak_bridge_*`, including
  `collectgarbage("stats")`, benchmark stat output, M8 weak guards, and M3
  weak-helper visibility checks. No old developer-stat aliases are kept.
- Removed two tombstone-only M3 CI guards for already-deleted weak/sweep phase
  aliases and old paranoia diff aliases. Current transition/root-diff behavior
  remains covered by C fixtures and documentation rather than helper comments.
- Removed duplicate M3 finalizer negative scans that M8 already owns through its
  close-time finalizer, callback-stack, and finalizer-spawn behavior gates. M3
  still keeps the positive worker-scheduler ownership checks.
- Renamed the GC2 lifecycle helper surface to purpose names:
  `lj_gc2_mark_begin()`, `lj_gc2_preserve_abort_to_idle()`, and
  `lj_gc2_cycle_to_idle()`. The remaining M3 notes describe the current lower-level
  cycle-close helper name instead of the removed fork-era label.
- Added persistent Lua-suite build-profile tracking so a new `lua_test.sh`
  process cannot silently reuse a previous alternate-XCFLAGS build, such as the
  JIT-disabled tail left by `m3_gc2_scaffold`, for default/JIT fixture cases.
  The new `m0_build_profile_switch` behavior test covers disabled-JIT to default
  profile recovery without relying on a legacy wrapper.
- Removed remaining stale old-name tombstones for deleted root/trace/finalizer
  wrapper names from CI invariant scripts where behavior tests already cover the
  publication/finalizer paths. Active `src`, `tests`, and `tools` now avoid
  standalone old-compat wording; the remaining `compatptr` identifiers are FFI
  type-conversion semantics and are intentionally retained.
- Public or semantic compatibility helpers such as FFI pointer compatibility
  checks are not removal targets unless the language/API contract changes.
- Anti-legacy wrappers that merely prevent reintroducing already-removed wrapper
  names should be deleted once compile, replacement-surface, and behavior
  coverage make them redundant.

Verification for the alias removal:

- `tools/ci/lua_test.sh --list`
- `tools/ci/lua_test.sh m5_profile_stop_native`
- `tools/ci/lua_test.sh m4_threading_api m4_threading_capi`
- `tools/ci/lua_test.sh m7_ffi_blocking m7_ffi_callback_runtime`
- `tools/ci/lua_test.sh m5_bcdump_current`
- `git diff --check`

## Next Refactors

1. Keep converting any remaining historical text-check contracts into
   behavior fixtures, generated dump/ASM checks, or documentation. Do not keep
   helper comments as tests, even for invariants that cannot be
   observed directly.
2. Do not add new pure shell aliases. If a case is fully Lua-owned, run it
   through `tools/ci/lua_test.sh <case...>`.
3. Split any large remaining GC/finalizer orchestration into suite-local helper
   modules so behavior tests live near the milestone cases they protect.
4. Revisit exact-name historical assertions after each implementation slice; when
   a contract is semantic rather than a required ABI, rewrite it around the
   semantic boundary or document it instead of checking source spelling.
