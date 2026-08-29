# GC phase fixture manual-step drain

`tests/t-arena-gcphase.c` now uses `lj_gc_step_explicit()` when it wants to
drive an active GC2/classic cycle to completion. The automatic
`lj_gc_step()` path is deliberately bounded to one state-machine step while
GC2 is active, so it is the wrong API for a fixture that is asserting the
post-cycle allocation-color invariant.

This is a test-harness correction only. The production automatic pacing path
still keeps active GC2 work bounded on mutator allocation checks, and the
public/manual step path keeps the stock-style completion behavior.
