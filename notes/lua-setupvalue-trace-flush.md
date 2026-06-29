# `lua_setupvalue()` trace invalidation

Lua closure upvalues marked immutable can be constified by the recorder. A later
`lua_setupvalue()` or `debug.setupvalue()` write to that upvalue is observable
Lua behavior, so existing traces must not keep returning the old constant.

The C-closure pseudo-index path already flushes traces for upvalue mutation.
Lua-upvalue stores now use the same mutation boundary when the raw `TValue`
actually changes, then publish the replacement cell with the existing release
store and GC publication wrapper.

Validation:

- `tools/ci/lua_test.sh m6_jit_cclosure_upvalue_flush`
