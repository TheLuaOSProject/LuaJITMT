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
payloads after publication, without relying on helper comments.

The fixture keeps periodic explicit GC out of the attached worker hot loop so it
stays focused on C-closure publication. Explicit active-thread `lua_gc()` from a
real attached pthread is covered by `tests/t-gc-active-collect-assist.c`: the
host thread brackets its raw `pthread_join()` with the runtime native boundary
so a GC2 safepoint handshake can remote-ack the waiting host TG. That native
wait contract is documented in `notes/threading-extension-surface.md`.

Validation:

- `tools/ci/lua_test.sh m5_upvalue_publish_gc`
