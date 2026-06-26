## GC2 assist and hard-check counter helpers

Mutator assist and hard-threshold telemetry is shared by interpreter GC checks,
trace-side GC checks, scoped trace flushing, stats export, and allocation-account
tests. This slice routes `assist_runs`, `assist_grey_drained`,
`assist_ssb_converted`, `assist_weak_drained`, `jit_hard_checks`,
`interp_hard_checks`, and `jit_scoped_slots_retired` through `gc2_*` helpers in
`lj_obj.h`.

Runtime initialization and increments now use the helper family, Lua GC stats
read assist counters through acquire helpers, and the focused allocation,
interpreter-hard-check, JIT-hard-check, and VM-safepoint fixtures read the same
counters through the helper surface. The M6 allocation-account guard now rejects
raw production C access to these assist and hard-check telemetry fields.

While touching stats export, the M6 cycle/root telemetry guard was tightened to
also reject local `GC2State *gc2` aliases. The remaining cycle/root stats export
reads now use the helper family as well.
