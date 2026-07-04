# Progress Report - 2026-06-28 Compatibility And CI

Scope: stock LuaJIT compatibility plus lockless runtime CI cleanup.

## Current Compatibility Boundary

- Stock LuaJIT public C API symbols and library behavior stay in scope.
- Fork-local threading APIs stay isolated under `require("threading")`.
- Bytecode compatibility with stock LuaJIT chunks is language/runtime behavior,
  not a threading-only migration detail.
- Mutable `ffi.cdef` remains serialized.

## Landed In This Area

- Current bytecode dump coverage checks load/execute behavior for supported
  stock and lockless dump versions, plus rejection of incompatible layouts.
- Public C compatibility aliases such as `luaL_putchar`, `lua_strlen`,
  `lua_open`, `lua_getregistry`, `lua_getgccount`, `lua_Chunkreader`, and
  `lua_Chunkwriter` remain available.
- FFI native-state and callback behavior moved into Lua/C fixtures.

## Coordination Inventory

Kept intentionally:

- User-facing synchronization APIs.
- Per-state owner claims.
- Safepoint leadership and GC2 worker lifecycle control.
- GDBJIT descriptor publication lock.
- FFI parser mutation serialization and conservative fallback waits around
  rollback windows.

## Verification At The Time

- `tools/ci/lua_test.sh m5_bcdump_current m5_registry_root`
- `tools/ci/lua_test.sh m5_parser_capture_meta m5_cell_ops m5_upvalue_publish_gc`
- `tools/ci/lua_test.sh m3_gc2_worker_scheduler`
- `tools/ci/lua_test.sh m8_weak`
- `git diff --check`
