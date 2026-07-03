# GC2 worker-active helper slice

This slice routes the GC2 scheduler's temporary single-worker claim token
through helper accessors:

- `gc2_worker_active_acq()` for peer waits in weak completion and mark
  completion.
- `gc2_worker_active_store_rlx()` for GC2 state initialization.
- `gc2_worker_active_rel()` for releasing the owner token after sweep,
  finalizer-drain, and worker-drain attempts.
- `gc2_worker_active_cas()` for the acquire-release owner claims in
  sweep-to-idle, finalizer draining, and worker draining.

The runtime users in `lj_gc2.c` no longer spell ad hoc atomics against
`GC2State.worker_active`. Runtime access to that scheduler ownership token must
stay behind the helper surface; the rule is documented here and covered through
worker/weak behavior instead of source-text matching.

Validation:

- `tools/ci/lua_test.sh m3_gc2_worker_scheduler`
- `tools/ci/lua_test.sh m8_weak`
- `git diff --check`
