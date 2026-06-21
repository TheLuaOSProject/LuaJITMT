# GC2 assist state helper slice

This slice moves the mutator-assist scheduler state behind helper accessors:

- `gc2_assist_shift_acq()` reads the current bounded-assist work shift.
- `gc2_assist_shift_store_rlx()` initializes the shift from `g->gc.stepmul`.
- `gc2_assist_shift_rel()` publishes public `collectgarbage("setstepmul")`
  changes.
- `gc2_assist_active_store_rlx()` initializes the nonblocking assist owner
  token.
- `gc2_assist_active_cas()` claims the current single assist owner with the
  existing acquire-release ordering.
- `gc2_assist_active_rel()` releases the owner token after the assist pass.

Production runtime users in `lj_api.c` and `lj_gc2.c` no longer spell ad hoc
loads, stores, or CAS operations against `GC2State.assist_shift` or
`GC2State.assist_active`. `tools/ci/m3_gc2_worker_scheduler.sh` rejects future
raw production access to those fields.

Validation:

- `tools/ci/m3_gc2_worker_scheduler.sh`
- `tools/ci/m6_jit_alloc_account.sh`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`
