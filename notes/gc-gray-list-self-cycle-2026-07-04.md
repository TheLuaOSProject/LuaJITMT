# GC Gray-List Self-Cycle

Date: 2026-07-04

`m7_ffi` exposed a full-GC hang in `tests/t-ffi-cdata-set-l.lua` at the
suite-sized `6 x 320` stress level. A smaller `6 x 80` run reproduced the hang:
the main thread was stuck in `collectgarbage("collect")`, repeatedly entering
`gc_onestep()` in `GCSpropagate`.

Debug sampling showed `g->gc.gray` pointing at the built-in `ffi.cast` fast
function and that object's `gclist` pointing back to itself. `propagatemark()`
blackened and popped the head, but `lj_gc_list_pop_head_rel()` restored the same
object as the head from its self-link, so the full collection never reached
`GCSpause`.

The fix hardens the intrusive legacy GC list helpers:

- pushing an object that is already the list head is a no-op, preventing a
  same-head duplicate mark from forming `o->gclist = o`;
- popping a self-linked head clears the list instead of preserving the cycle;
- `gc_mark()` now returns in release builds if an invalid non-white mark slips
  through, while assertion builds still report the invariant violation.

The M7 cdata-set wrappers now run the hang-prone script under an explicit
timeout, so future regressions fail instead of blocking the aggregate suite.
