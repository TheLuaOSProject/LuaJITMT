# ffi.pin removal

`ffi.pin` was a fork-local public FFI entrypoint used as a convenience root for
Lua values and cdata. It is not part of stock LuaJIT's FFI API and is not a
threading namespace primitive.

The fork now removes that public API surface and its dedicated handle userdata.
The stock-visible lifetime mechanisms remain `ffi.gc`, ordinary Lua references,
and the existing finalizer machinery. Tests should validate those behaviors
directly instead of carrying a non-stock helper or a source-search guard for it.

Validation:

- `tools/ci/lua_test.sh m5_stock_api_surface run_stock_tests`
- `tools/ci/lua_test.sh m7_ffi_finreg m7_ffi_metatype`
