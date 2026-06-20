GC sweep acquire-root slice

- `gc_sweep()` now acquire-loads the next object from the sweep list with
  `gcref_acq()`, matching `gc_sweepstr()` and the newer root/FINREG unlink
  paths.
- Sweep root-anchor comparisons now acquire-load `g->gc.root` before deciding
  whether the root anchor needs adjustment.

Verification:

- tools/ci/lua_test.sh m3_gc2_paranoia
- tools/ci/lua_test.sh m8_weak
