# GC2 generational gate helper slice

This slice routes GC2 generational mode and minor-cycle gate state through
field-specific helper accessors:

- `gc2_generational_*()` publishes and reads the public generational mode bit.
- `gc2_force_major_*()` publishes and consumes the one-shot major override.
- `gc2_cycle_minor_requested_*()`, `gc2_cycle_sweep_minor_*()`, and
  `gc2_cycle_roots_minor_*()` publish and read the per-cycle minor latches.
- `gc2_minor_sweep_enabled_*()` and `gc2_minor_roots_enabled_*()` publish and
  read the public minor sweep/root gates.
- `gc2_minor_survival_pct_*()` and
  `gc2_minor_survival_threshold_pct_*()` publish and read the minor survival
  policy scalars.

Runtime users in `lj_gc2.c`, the legacy GC bridge, TG attach catch-up, the
public `lua_gc()` mode controls, and `collectgarbage("stats")` now call helper
accessors instead of spelling ad hoc atomics against those `GC2State` words.
`tools/ci/m10_generational.sh` rejects future raw production access in
`lj_api.c`, `lj_gc.c`, `lj_gc2.c`, `lj_tg.c`, and `lib_base.c` while leaving
the helper bodies as the single raw-access point.

Validation:

- `tools/ci/m10_generational.sh`
- `tools/ci/m9_gc_stats.sh`
- `tools/ci/m6_jit_alloc_account.sh`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`
