# GC2 bounded fixpoint fixture

`t-gc2-phase.c` no longer assumes one `lj_gc_step()` closes every edge found by
a GC2 root scan. The fixpoint bridge is deliberately bounded by
`LJ_GC2_WORKER_DRAIN_BATCH`: a round can mark the stack root and still leave
grey work behind when the full root scan has queued unrelated library, FFI, or
JIT roots ahead of the fixture's table graph.

The fixture now asserts the stricter contract that matters for the collector:
the first step performs one bounded fixpoint round and marks the stack root; if
the child chain is not closed yet, GC2 must still report non-empty work. Further
bounded steps must mark the child and grandchild while the collector remains in
MARK.

This is a test stability fix, not a relaxed collector rule. Direct
`lj_gc2_fixpoint_round(..., ~(uint32_t)0)` coverage in `t-gc2-traverse.c` still
proves that an unbounded round closes the small parent/child/grandchild graph
before the following zero-mark hit.
