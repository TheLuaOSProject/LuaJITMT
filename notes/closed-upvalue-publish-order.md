# Closed upvalue root publication order

Closed `GCupval` constructors now allocate raw/unlinked storage, initialize the
complete object body, mark it white, run any initialized-object edge barrier,
and only then publish the object to the pending root chain.

The payload barrier is required only when the copied closed value is itself a
GC reference. Numeric, boolean, nil, and lightuserdata captures have no child
edge, so `func_pubuv_payload()` leaves them on the fast path without taking the
TValue publication helper. GC-valued captures still run the same GC2 and legacy
barrier before the upvalue becomes reachable through pending-root publication.

This removes the old `lj_mem_newgco()` window where a pending-root flusher could
observe an untyped or partially initialized closed upvalue. Open upvalues still
use their separate open-upvalue list protocol. When a legacy open upvalue is
closed, `lj_func_closeuv()` first unlinks it from the thread-open chain and
`g->uvhead`; only then does `lj_gc_closeuv()` queue it on the pending
object-list bridge. That is safe because closed-upvalue liveness is marked
through closures that reference the `GCupval`, while object-list publication
only has to happen before sweep/free-list consumers, all of which flush pending
roots.

`m5_upvalue_publish_gc` owns the behavior coverage for publication order.
Closed upvalues must not return to immediate-link allocation: initialization,
barrier repair, and pending-root publication stay ordered by the constructor
comments and the focused fixture rather than by legacy wrappers.
`m3_gc_root_pending` also covers the legacy-open close path and checks that the
closed `GCupval` reaches the pending queue before the next flush publishes it to
the legacy sweep spine.
