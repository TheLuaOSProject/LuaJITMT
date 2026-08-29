# FNEW Active Constructor Barrier Filter

The active GC2 marked-child filter is intentionally local to fresh FNEW bump
constructors. A global `lj_gc2_barrier_obj_pair()` skip was rejected in
`gc2-active-barrier-filter-audit-2026-07-04.md` because a marked child alone
does not prove that a broad write barrier edge is already owned.

The FNEW bump case has a narrower proof:

- The parent closure is freshly initialized and unpublished until the pending
  root chain release store.
- During active black allocation, the fresh function and fresh local-cell
  upvalue already have their arena mark bits set before any edge can publish.
- The other constructor children are the current proto and environment, which
  are established roots for the running frame/trace.
- Active white allocation, unmarked/custom children, and idle remembered-set
  publication still use the normal barrier path.

This removes repeated locked mark updates for `proto`, `env`, and fresh
one-upvalue constructor edges during long active-GC closure-allocation loops.
The focused stock guard moved `closures_upval` at `BENCH_SCALE=0.2` from about
`2.35x` stock to about `2.16x` on the local Linux/x64 machine. The remaining
gap is mostly active-cycle duration and allocation-color overhead, not the basic
FNEW bump allocator: short-scale and GC-stopped runs are already near or faster
than stock.
