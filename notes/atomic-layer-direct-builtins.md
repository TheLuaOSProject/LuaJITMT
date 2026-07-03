# Atomic layer direct builtin cleanup

This slice removes the remaining direct production uses of GCC atomic builtins
that were outside `src/lj_atomic.h`.

- GC total relaxed add/sub helpers now use `la_add*_rlx()` and
  `la_sub*_rlx()`.
- Global hook function load/store now use typed function-pointer helpers from
  `lj_atomic.h`, preserving the existing acquire/release publication.
- The opt-in GDBJIT descriptor spinlock now uses `la_cas32()` plus
  `la_store32_rel()` instead of legacy `__sync_*` builtins.
- The same opt-in descriptor lock now sleeps in no-`lua_State` native slices
  under contention rather than burning CPU in a pause loop.
- Focused regression tests now reject regressions in GC total accounting, global hook
  function publication, and the GDBJIT synchronization surface.

Validation:

- `tools/ci/m5_gc_total_atomic.sh`
- `tools/ci/m5_hook_state_atomic.sh`
- `tools/ci/m6_jit_gdbjit_publish.sh`
