# Progress Report - 2026-06-28 C-Closure And Coordination

Scope: x86_64/Linux `v2.1` lockless LuaJIT fork. Priority remains stock
semantics, memory safety, and stability before performance parity.

## Landed In This Area

- Public C API reads of C-closure upvalue pseudo-indices use acquire snapshots.
- String coercions through C-closure upvalues release-publish the updated
  `TValue`.
- `string.gmatch` hidden upvalues use full-value acquire snapshots, and the
  iterator position update uses a full primitive release store.
- `tests/t-cclosure-upvalue-snapshot.c` covers public API reads/writes,
  coercions, table/metatable/env paths, nested upvalue APIs, userdata/thread
  reads, and `string.gmatch` debug mutation.

## Coordination Inventory

Kept intentionally:

- `threading.mutex`, channels, joins, and sleeps are user-visible blocking
  semantics.
- Per-`lua_State` owner claims prevent concurrent mutation of a coroutine.
- Safepoint leadership coordinates GC, shutdown, redispatch, and trace flush.
- FFI parser serialization remains required for mutable `ffi.cdef` and rollback
  windows.
- GDBJIT descriptor locking protects debugger-facing metadata.

Worth shrinking later:

- FFI read fallback waits where a sequence-checked snapshot/refetch helper can
  preserve normal behavior.
- Table claim and resize waits after stress coverage proves the publication
  boundary.
- GC2 bridge tokens where behavior coverage proves finalizer, weak-table, and
  sweep ownership.

## Verification At The Time

- `tools/ci/lua_test.sh m5_upvalue_publish_gc`
- `tools/ci/lua_test.sh m4_threading_upvalue`
- `tools/ci/lua_test.sh m5_cell_ops`
- Stock string suite under the built VM.
