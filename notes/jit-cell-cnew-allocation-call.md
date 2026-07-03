# JIT Local-Cell CNEW Allocation Call

Traced `BC_CNEW` local-cell creation now records
`lj_func_newuvcell_forjit()` as an allocation call. x64 trace assembly therefore
owns the allocation pacing check before the helper runs, matching traced
`FNEW`, `TNEW`, `SNEW`, and FFI `CNEW` allocation helpers.

The interpreter-facing `lj_func_newuvcell()` still runs `lj_gc_check_fixtop()`.
The traced helper bypasses that wrapper and constructs the closed nil upvalue
directly, avoiding a second pacing predicate in local-cell creation traces.

Recorder CNEW reuse now scans both allocation-call and side-effect-call chains,
so same-trace `CGET`/`CSET` after a freshly emitted `BC_CNEW` still reuses the
new cell reference. Coverage is behavioral: `m6_jit_cell_ops` exercises parsed
and loaded `CNEW`/`FNEW` traces, and `m6_jit_gc2_readiness` includes a
local-cell creation trace in the GC2 hard-threshold fixture. No source, IR dump,
ASM dump, or mcode-byte guard is used.
