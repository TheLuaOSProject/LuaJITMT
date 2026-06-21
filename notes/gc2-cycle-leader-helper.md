# GC2 cycle-leader helper slice

This slice routes the GC2 nonblocking cycle-request token through helper
accessors:

- `gc2_cycle_leader_store_rlx()` initializes the token to idle.
- `gc2_cycle_leader_cas()` claims cycle leadership with the existing
  acquire-release CAS.
- `gc2_cycle_leader_xchg_acqrel()` consumes and clears the pending request at
  mark begin.
- `gc2_cycle_leader_rel()` clears stale requests on preserve-abort,
  sweep-to-idle, and legacy cycle-end paths.

Production runtime users in `lj_gc2.c` no longer spell ad hoc atomics against
`GC2State.cycle_leader`. `tools/ci/m3_gc2_worker_scheduler.sh` rejects future
raw production access to the token.

Validation:

- `tools/ci/m3_gc2_worker_scheduler.sh`
- `tools/ci/m6_jit_alloc_account.sh`
- `tools/ci/m10_generational.sh`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`
