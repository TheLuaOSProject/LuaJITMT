# C Closure Upvalue Publication Race

`tests/t-cclosure-upvalue-race.c` covers the shared-object boundary that matters
for fork threading: one attached OS thread repeatedly republishes a shared C
closure upvalue with `lua_setupvalue()` and with the C-closure pseudo-index
mutation APIs (`lua_replace(lua_upvalueindex(1))` and
`lua_copy(..., lua_upvalueindex(1))`), while other attached states call the
same closure through the public C API.

Each replacement upvalue is a fully populated table with redundant fields and a
self-reference. Readers accept any old or new sequence number, but they require
the table, nested table, checksum, tag, and self-reference to agree. This keeps
the check behavioral: the public API must only expose fully formed upvalue
payloads after publication, without relying on repository source searches.

The fixture intentionally leaves periodic explicit GC out of the attached
worker hot loop. A temporary stress version with `lua_gc(..., LUA_GCSTEP, ...)`
inside the writer/reader loop exposed a separate safepoint issue: an attached
worker can start a GC2 handshake while a host thread is blocked in
`pthread_join()` outside the runtime and therefore cannot acknowledge. That is a
native-state/embedding follow-up, not part of the C-closure publication proof.

Validation:

- `tools/ci/lua_test.sh m5_upvalue_publish_gc`
