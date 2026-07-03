# Closed upvalue root publication order

Closed `GCupval` constructors now allocate raw/unlinked storage, initialize the
complete object body, mark it white, run any initialized-object edge barrier,
and only then publish the object to the pending root chain.

This removes the old `lj_mem_newgco()` window where a pending-root flusher could
observe an untyped or partially initialized closed upvalue. Open upvalues still
use their separate open-upvalue list protocol.

`m5_upvalue_publish_gc` owns the behavior coverage for publication order.
Closed upvalues must not return to immediate-link allocation: initialization,
barrier repair, and pending-root publication stay ordered by the constructor
comments and the focused fixture rather than by implementation-text assertions.
