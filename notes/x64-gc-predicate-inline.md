# x64 GC Predicate Inline

- Replaced the x64 VM `lj_gc_should_step_vm()` calls in `ffgccheck`,
  `BC_TNEW`, and `BC_TDUP` with a VM-local predicate:
  `gc.total >= gc.threshold || gc2.alloc_since_trigger > gc2.hard_bytes`.
- This preserves the GC2 hard-limit assist condition that motivated the C
  helper, while removing a C call from the no-GC-work fast path for table
  allocation and string fast-function allocation checks.
- This is not the plan's final table allocation path. Exact-empty x64 `TNEW`
  now has its own guarded arena-bump body, accounting, and pending-root
  publication path. Non-empty `TNEW` and `TDUP` still allocate through the C
  table helpers, so their object initialization, publication, accounting, and
  table/template semantics stay centralized there.
- Verification:
  - `make -C src -j2`
  - `tools/ci/lua_test.sh m6_jit_alloc_account m6_jit_gcstep_pacing m2_arena_alloc m2_arena_gcphase`
  - `tools/ci/lua_test.sh m9_bench_smoke`
