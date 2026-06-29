# `lua_setupvalue()` trace invalidation

Lua closure upvalues marked immutable can be constified by the recorder. A later
`lua_setupvalue()` or `debug.setupvalue()` write to that upvalue is observable
Lua behavior, so existing traces must not keep returning the old constant.

The C-closure pseudo-index path already flushes traces for upvalue mutation.
Lua-upvalue stores now use the same mutation boundary when the raw `TValue`
actually changes, then publish the replacement cell with the existing release
store and GC publication wrapper.

The mutation boundary first checks for live or actively recording traces before
requesting the global trace-flush handshake. This keeps the stock-visible
invalidation rule intact when traces exist, while avoiding a pointless
safepoint handshake in C API programs that have no trace state to invalidate.

Validation:

- `tools/ci/lua_test.sh m6_jit_cclosure_upvalue_flush`
- `tools/ci/lua_test.sh m5_upvalue_publish_gc`
