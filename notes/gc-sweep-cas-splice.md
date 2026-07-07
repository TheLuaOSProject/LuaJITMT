GC sweep CAS-splice slice

- `gc_sweep()` now uses the existing `gc_chain_splice()` CAS unlink helper for
  dead objects and already-destructed GC2 arena bodies.
- This removes the remaining raw root/openupvalue-list rewrite in the legacy
  sweep loop while preserving the same retry-at-current-link traversal shape.

Verification:

- tools/ci/lua_test.sh m2_arena_gcsweep
- tools/ci/lua_test.sh m3_gc2_paranoia
- tools/ci/lua_test.sh m8_weak
- tools/ci/lua_test.sh m9_m10_gc

Follow-up:
- Legacy root-list splice, prepend, after-anchor publish, and close-state unlink
  paths now use the shared `gcref_cas()` helper instead of spelling GC64/GC32
  pointer CASes at each call site. The retry shape is unchanged, but root-list
  CAS publication now shares the same object-reference helper surface as FINREG
  and pending-root helpers.
- Verification: clean build, `m2_arena_gcsweep`, `m3_gc2_paranoia`, `m8_weak`,
  and `m10_generational` passed.
