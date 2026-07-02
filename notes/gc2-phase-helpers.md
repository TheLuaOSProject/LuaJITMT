# GC2 phase helper slice

This slice routes production access to the authoritative GC2 phase word through
helper accessors:

- `gc2_phase_acq()` for phase predicates in GC2, legacy GC bridge code, and TG
  attach catch-up.
- `gc2_phase_store_rlx()` for initialization to idle.
- `gc2_phase_rel()` for mark-begin publication.
- `gc2_phase_cas()` for MARK-to-WEAK and WEAK-to-SWEEP transitions.
- `gc2_phase_xchg_acqrel()` for preserve-abort, sweep-to-idle, and legacy
  cycle-end IDLE exchanges.

Runtime users in `lj_gc2.c`, `lj_gc.c`, and `lj_tg.c` no longer spell ad hoc
atomics against `GC2State.phase`. `tools/ci/m3_gc2_worker_scheduler.sh` rejects
future raw production access to the phase word in those files while leaving the
helper bodies as the single raw-access point.

Validation:

- `tools/ci/m3_gc2_worker_scheduler.sh`
- `tools/ci/m6_jit_alloc_account.sh`
- `tools/ci/m8_weak.sh`
- `tools/ci/m10_generational.sh`
- `git diff --check`
