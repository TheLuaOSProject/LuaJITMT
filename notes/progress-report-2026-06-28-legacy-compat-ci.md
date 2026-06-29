# Progress report - 2026-06-28 legacy/compat and CI cleanup

Scope: x86_64/Linux `v2.1` lockless LuaJIT fork. Current policy is safety,
stability, and Lua semantics first; LuaJIT performance parity is secondary when
those goals conflict.

## Current estimate

Overall safety/stability completion: 66-76%.

- Runtime lockless substrate: 72-82%.
- Threading/channel/shutdown behavior coverage: 78-88%.
- CI migration from legacy shell/source guards to behavior tests: 60-70%.
- FFI concurrency excluding mutable `ffi.cdef`: 58-68%.
- Weak/finalizer/generational GC: 55-68%.
- Release-quality soak and benchmark readiness: 45-55%.
- Performance parity with LuaJIT: 35-45%.

Time remaining forecast:

- Correctness alpha: about 2-4 focused weeks.
- Strong beta with broader stress and JIT/FFI closure: about 6-10 focused weeks.
- Performance pass: about 4-10 additional weeks after semantic closure.
- Production-confidence soak: about 3-6 months of workload validation.

## Done in this slice

- Removed pre-lockless bytecode dump compatibility: the loader now accepts only
  the current lockless dump version.
- Deleted the `proto_legacyuv` compatibility path and the writer-side legacy
  upvalue walk.
- Renamed the old bytecode fixture to `m5_bcdump_current`; it now verifies
  current-format loading and malformed current dump rejection.
- Reverted the removal of stock LuaJIT public C compatibility aliases such as
  `luaL_putchar`, `lua_strlen`, `lua_open`, `lua_getregistry`,
  `lua_getgccount`, `lua_Chunkreader`, and `lua_Chunkwriter`; stock LuaJIT API
  compatibility stays in scope.
- Slimmed `tools/ci/m7_ffi_blocking.sh` so behavior-covered blacklist and
  recorder checks are no longer pinned to exact source shape.

## Locks and coordination

No broad global VM lock has been found outside intentionally exposed sync APIs.
Remaining locks/waits are mostly good-faith safety mechanisms:

- `threading.mutex`, channels, and joins: user-visible blocking semantics.
- Per-state owner claims: prevent concurrent mutation of one Lua state.
- Safepoint leadership and GC2 worker lifecycle control: shutdown/GC safety.
- GDBJIT descriptor lock: debugger publication, low hot-path value.
- FFI parser mutation serialization and conservative fallback locks: still
  needed around rollback and abandoned-entry windows.

Worth making more lockless:

- FFI read fallback locks, one snapshot helper at a time.
- Table resize/wait stress and proof coverage before simplifying waits.
- Traced FFI native-state protocol, while traced calls remain disabled.

Not worth removing now:

- Explicit synchronization APIs, safepoint leadership, GC2 lifecycle parking,
  GDBJIT locking, and mutable `ffi.cdef` serialization.

## Test and CI audit

Keep source guards only for non-observable memory-order contracts, ABI fences,
or temporary contracts with a behavior-test TODO. Best near-term cleanup:

- Move/delete pure source-shape checks in M5 x64, M7 FFI, and M6 JIT where
  generated output or runtime behavior already proves the contract.
- Split `t-gc2-traverse.c` or add selectors; it is still a monolithic mixed
  fixture used by M3 and M8.
- Deduplicate overlapping finalizer ownership lints between M3 and M8 shell
  guards.
- Add explicit timeouts to remaining intentionally blocking threading cases.

## Verification

Passed:

- `tools/ci/lua_test.sh m5_bcdump_current m5_registry_root`
- `tools/ci/lua_test.sh m5_parser_capture_meta m5_cell_ops m5_upvalue_publish_gc`
- `tools/ci/lua_test.sh m3_gc2_worker_scheduler`
- `tools/ci/lua_test.sh m8_weak`
- `tools/ci/m7_ffi_blocking.sh`
- `git diff --check`
