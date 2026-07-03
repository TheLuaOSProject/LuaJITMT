# Closed local-cell bump allocation

`BC_CNEW` creates a closed `GCupval` initialized to nil for mutable local-cell
captures. The generic path is still the fallback, but the ordinary main-thread
window now uses the same TG-local traversable-arena bump allocator as the
existing FNEW fast paths.

The bump path is intentionally narrow:

- no secondary thread is active or entering,
- no GC2 worker is running,
- the current thread group is the main thread group and owns the arena allocator,
- the TG-local allocation account has room before the next flush,
- no owner-local free run can satisfy the allocation first.

Those constraints keep the helper off contended warm paths and preserve the
allocator reuse order expected by the arena free-run path. Interpreter `BC_CNEW`
still runs `lj_gc_check_fixtop()` before allocation; traced `BC_CNEW` is already
recorded as an allocation helper, so trace assembly owns the allocation pacing
check before calling `lj_func_newuvcell_forjit()`.

Coverage is behavior-based. `m6_jit_fnew_bump` builds with
`LJ_FUNC_TEST_HELPERS` and directly exercises both interpreter and traced helper
entry points, checking that they produce closed nil upvalue cells and publish the
cell through the destination slot where required. It does not inspect source,
generated IR, assembly, or generated mcode encoding.
