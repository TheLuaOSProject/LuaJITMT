# C Closure Upvalue Publication Race

`tests/t-cclosure-upvalue-race.c` covers the shared-object boundary that matters
for fork threading: one attached OS thread repeatedly republishes a shared C
closure upvalue with `lua_setupvalue()`, while other attached states call the
same closure through the public C API.

Each replacement upvalue is a fully populated table with redundant fields and a
self-reference. Readers accept any old or new sequence number, but they require
the table, nested table, checksum, tag, and self-reference to agree. This keeps
the check behavioral: the public API must only expose fully formed upvalue
payloads after publication, without relying on repository source searches.

Validation:

- `tools/ci/lua_test.sh m5_upvalue_publish_gc`
