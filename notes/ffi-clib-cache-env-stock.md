# FFI CLibrary cache environment stock boundary

Stock LuaJIT keeps a per-`CLibrary` cache table and initially exposes that table
as the userdata environment. Mutating that original table can override
`ffi.C.symbol` or `ffi.load(...).symbol`, but replacing the userdata environment
with `debug.setfenv(clib, newenv)` does not redirect lookup and does not cause
future cache hits or misses to populate `newenv`.

The lockless side cache must preserve that behavior. `CLibrary.cache_env` is the
marked original cache table. Interpreter lookup, recorder specialization, and
cache mirroring use `cache_env`; the mutable userdata env remains regular Lua
metadata and is still marked by the collectors, but it is not consulted for
CLibrary symbol lookup after replacement.

Coverage:

- `tests/t-ffi-clib-cache.lua` compares original-cache overrides with
  replacement-env overrides for `ffi.C` and `ffi.load("c")`.
- The same test heats a replacement-env `ffi.C.abs` loop to ensure the recorder
  ignores replacement env overrides and records the stock CLibrary result.

Verification:

- `tools/ci/lua_test.sh m7_ffi_clib_cache`
- `tools/ci/lua_test.sh m3_interp_stock_joff m7_ffi_clib_ldscript m7_ffi_ccall_native`
