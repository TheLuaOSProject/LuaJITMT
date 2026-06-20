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
