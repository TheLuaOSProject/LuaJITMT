Weak bridge acquire-root slice

- `lj_gc_atomic()` now snapshots `g->gc.weak` with an acquire load once and
  passes that same head to both `lj_gc2_weak_complete()` and the bridge fallback
  clear.
- `lj_gc_clearweak_bridge()` now follows `GCtab.gclist` with `gcref_acq()`,
  matching the GC2 weak coverage/backfill walkers.

Verification:

- tools/ci/lua_test.sh m8_weak
- tools/ci/lua_test.sh m9_m10_gc
