# `debug.upvalueid()` C closure bounds

`debug.upvalueid()` remains a stock-compatible public debug API: Lua closures
return their shared `GCupval` identity and C closures return the address of the
inline upvalue cell.

The C-closure branch now bounds-checks through `GCfuncC.nupvalues` instead of
the Lua-function union view before returning a C upvalue cell. This is not a
surface removal; it keeps the existing API behavior while making the
threading-sensitive shared-slot path explicit.

Validation:

- `tools/ci/lua_test.sh m5_upvalue_publish_gc`
