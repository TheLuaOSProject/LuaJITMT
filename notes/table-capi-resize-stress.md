# Public C API table resize stress

The existing table fixtures cover direct helper retries over forwarded array and
hash slots. `tests/t-tab-capi-resize-stress.c` adds a public C API fixture for
the same shared-object boundary: attached OS threads mutate one shared table
through `lua_rawseti()`, `lua_settable()`, `lua_rawset()`, and `lua_setfield()`
while their writes force array/hash growth and deletion churn.

The fixture verifies final per-thread keys and a table-owned sentinel value
after full GC. This keeps the invariant behavioral: public C setters must route
through the forwarding/resize publication protocol without exposing sentinels,
losing final writes, or dropping table-owned objects. No repository source
search is part of the check.

Validation:

- `tools/ci/lua_test.sh m5_tab_capi_resize_stress`
