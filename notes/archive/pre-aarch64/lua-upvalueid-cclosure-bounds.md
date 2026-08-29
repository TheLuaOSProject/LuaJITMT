# `lua_upvalueid()` C closure upvalue bounds

`lua_upvalueid()` acquire-snapshots the target function slot before deriving an
upvalue identity. Lua closures identify a `GCupval` object, while C closures
identify the address of the inline `TValue` cell in the C closure object.

Both closure forms store their upvalue count in the common function header, but
the API now checks Lua and C closures through their matching union views before
returning the corresponding identity pointer. This keeps the C-closure path
explicit and avoids future confusion with Lua-only `GCupval` storage.

`tests/t-cclosure-upvalue-snapshot.c` covers this with a nested C closure that
has two upvalues. The fixture reads both cells through `lua_getupvalue()`,
verifies `lua_upvalueid()` returns distinct identities, replaces only the second
cell with `lua_setupvalue()`, and then calls the carrier closure to confirm the
first cell was left untouched.

Validation target:
- `tools/ci/lua_test.sh m5_upvalue_publish_gc`
