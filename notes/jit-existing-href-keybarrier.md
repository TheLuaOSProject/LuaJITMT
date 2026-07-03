# JIT existing HREF key-barrier elision

Dynamic string-key stores to an existing non-nil hash slot recorded as `HREF`
used to keep a key-only `TBAR` even when the stored value was primitive. The
recorder already emitted an `HREF != niltv` guard in exactly this uncertain
case, so the trace had proven that the slot was an existing non-nil entry before
the store.

That proof is enough for strong tables: the key edge was established by the
original insertion, and a later primitive value update does not create a new GC
edge. It is also required for weak-key tables: a numeric update must not turn a
weak key into a synthetic strong edge through `lj_gc2_barrier_key_g()`.

`lj_record_idx()` now clears the key-barrier request when it emits the runtime
non-nil `HREF` guard. New-key and previous-nil paths still use `NEWREF` and keep
their existing publication/barrier rules. GC-valued updates still emit the
normal table barrier for the value edge.

Coverage: `m6_jit_tbar_gc2_black_gate` now dumps a dynamic existing-key HREF
store trace and verifies that the guarded primitive update records `HSTORE`
without `TBAR`, while the existing runtime weak-key check verifies that key-only
barriers do not keep weak keys alive.
