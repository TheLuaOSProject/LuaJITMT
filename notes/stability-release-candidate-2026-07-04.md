# Stability release-candidate slice - 2026-07-04

This slice stabilized the Linux release gate after the FFI and GC/JIT active-root
changes.

Runtime fixes:

- Generated library `SET` records now snapshot stack key/value operands before
  lockless table insertion and emit the weak-write barrier from the helper. This
  fixed source-only FFI initialization where `ffi.C` could be corrupted during
  library registration.
- Legacy `gc_mark()` now atomically claims white bits before traversal so
  duplicate lockless roots/barriers do not trip assertion builds or build
  duplicate grey-list entries.
- GC2 and legacy GC root scanning include per-TG JIT/native temporary TValue
  slots while a TG is executing native/JIT code.
- GC2 mark/weak/sweep/finalizer transitions defer while any TG is active in
  JIT/native code, avoiding phase changes through owner-private frames.
- Busy thread NEEDSCAN handoff accounting now counts the pending handoff as the
  requeue work item. The grey deque no longer needs a duplicate item for that
  state.

Harness/test fixes:

- The suite runner disables JIT only for the control-plane harness process.
- C fixture helpers snapshot run options before clean builds and compiles so
  long aggregate gates preserve fixture environment variables and timeouts.
- GC2 thread traversal tests now distinguish dead-owner, live remote-owner, and
  same-owner cases. The live remote-owner case uses a narrow test helper to scan
  owned NEEDSCAN states without letting a synthetic TG participate in global
  safepoint handshakes.

Verification:

- `tools/ci/lua_test.sh m3_gc2_paranoia`
- `tools/ci/lua_test.sh m8_weak`
- `tools/ci/lua_test.sh m7_ffi`
- `tools/ci/lua_test.sh m6_jit`
- `tools/ci/lua_test.sh run_stock_tests -- --quiet`
- `git diff --check`
