# Closed upvalue root publication order

Closed `GCupval` constructors now allocate raw/unlinked storage, initialize the
complete object body, mark it white, run any initialized-object edge barrier,
and only then publish the object to the pending root chain.

This removes the old `lj_mem_newgco()` window where a pending-root flusher could
observe an untyped or partially initialized closed upvalue. Open upvalues still
use their separate open-upvalue list protocol.

Guard: `m5_upvalue_publish_gc` source-checks `lj_func.c` so closed upvalues do
not return to immediate-link allocation and the initialization/barrier/link
ordering remains explicit.
