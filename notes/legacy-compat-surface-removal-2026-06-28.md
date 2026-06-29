# Legacy and compatibility surface removal

This cleanup is scoped to stale fork-local/threading compatibility entry points
that conflicted with the current lockless policy. It must not remove stock
LuaJIT public API, language behavior, or generally supported stock entry points.

- Deleted unsupported Windows and console batch build scripts.
- Rewrote `doc/install.html` for the only supported target: x86-64 Linux with
  GC64.
- Updated public docs and Makefiles so they no longer advertise MSVC/MinGW,
  console, non-x64, or GC32 builds.
- Kept stock `-DLUAJIT_ENABLE_LUA52COMPAT` handling in `lj_arch.h`. The
  cleanup scope excludes stock optional language/library compatibility mode;
  only stale fork-local/threading compatibility entry points should be removed.
- Removed stale v2 bytecode compatibility plan/test material and replaced it
  with current-only dump validation wording.
- Kept stock LuaJIT public API compatibility entry points and macros, including
  `luaL_openlib()`, `luaL_register()`, `luaL_typerror()`, `luaL_checkint`,
  `luaL_optint`, `luaL_checklong`, `luaL_optlong`, `luaL_putchar`,
  `lua_strlen`, `lua_open`, `lua_getregistry`, `lua_getgccount`,
  `lua_Chunkreader`, and `lua_Chunkwriter`. These are part of the stock LuaJIT
  API surface and are not old fork-specific compatibility wrappers.
- Removed redundant `string.gmatch` C-closure and local-cell x64 source guards;
  behavior fixtures, bytecode checks, and generated JIT dump checks cover the
  observable semantics.
- Removed the public `ffi.blocking()` compatibility marker after ordinary FFI
  calls were made native-state safe by default. Internal callback blacklisting
  remains for callback safety.
- Internalized the GC2 generational/incremental mode selector values so
  `LUA_GCGENERATIONAL`, `LUA_GCINCREMENTAL`, and matching `lua_gc()` numeric
  modes are not exported as public C API. The stock `lua_gc()` behavior for
  unknown modes remains `-1`.
- Moved fork-local GC2 Lua controls off base `collectgarbage()` and into the
  explicit threading extension. `notes/threading-extension-surface.md`
  enumerates the kept fork-local controls, including `threading.now()`,
  `threading.gcstats()`, `threading.gcworkers()`, and `threading.gcmode()`.
  This keeps stock `collectgarbage()` option parsing intact while preserving
  the lockless runtime's diagnostics and tuning hooks.

Kept intentionally:

- Stock LuaJIT public C API entry points and macros. The old-compat cleanup
  target is fork-specific threading/lockless compatibility surface, not the
  stock LuaJIT API contract.
- GC "legacy" bridge names and weak fallback counters while they describe real
  safety bridge behavior.
- FFI C type compatibility helpers; these are language/FFI semantics, not
  old public C API aliases.

Verification:

- `tools/ci/lua_test.sh m0_lua52_compat`
- `tools/ci/lua_test.sh m5_bcdump_current m5_upvalue_publish_gc`
- `tools/ci/lua_test.sh m5_cell_ops m6_jit_cell_ops`
- `make -C src XCFLAGS=-DLUAJIT_ENABLE_LUA52COMPAT` builds the optional stock
  Lua 5.2 compatibility profile.
- `make -C src -j`
