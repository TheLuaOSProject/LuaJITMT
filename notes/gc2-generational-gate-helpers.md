# GC2 generational gate helper slice

This slice routes GC2 generational mode and minor-cycle gate state through
field-specific helper accessors:

- `gc2_generational_*()` publishes and reads the fork-local generational mode
  bit.
- `gc2_force_major_*()` publishes and consumes the one-shot major override.
- `gc2_cycle_minor_requested_*()`, `gc2_cycle_sweep_minor_*()`, and
  `gc2_cycle_roots_minor_*()` publish and read the per-cycle minor latches.
- `gc2_minor_sweep_enabled_*()` and `gc2_minor_roots_enabled_*()` publish and
  read the minor sweep/root gates.
- `gc2_minor_survival_pct_*()` and
  `gc2_minor_survival_threshold_pct_*()` publish and read the minor survival
  policy scalars.

Runtime users in `lj_gc2.c`, the legacy GC bridge, TG attach catch-up, the
fork-local `collectgarbage()` mode controls, and `collectgarbage("stats")` now
call helper accessors instead of spelling ad hoc atomics against those
`GC2State` words. This is a documented ownership boundary rather than a
source-shape CI guard; behavior coverage exercises the Lua-visible transitions,
and the stock C API fixture keeps the non-stock `lua_gc()` numeric modes
rejected.

Validation:

- `tools/ci/lua_test.sh m10_generational`
- `tools/ci/lua_test.sh m9_gc_stats`
- `tools/ci/lua_test.sh m6_jit_alloc_account`
- `git diff --check`
